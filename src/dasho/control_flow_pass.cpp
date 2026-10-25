#include "dasho/control_flow_pass.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "dasho/cfg.hpp"
#include "dasho/class_set.hpp"
#include "dasho/dead_class.hpp"

namespace dasho {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using jbc::core::Op;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

namespace {

/// DashO 生成类的统一 classfile 版本：**45.3**（major 45 / minor 3）。
///
/// 打包口径与 `ClassUnit::version` 一致 = (major<<16)|minor。注意 classfile
/// 文件里先写 minor 再写 major，把它俩按文件顺序读成一个 u32 会得到 196653
/// （= 3<<16|45），与这里的 2949123 是同一个版本号的两种读法——不要混用。
constexpr ca::u32 kGeneratedVersion = (45u << 16) | 3u;

/// 假异常类的直接父类。DashO 注入的 `{Owner}$Exception` 系全部直挂它。
constexpr std::string_view kRuntimeException = "java/lang/RuntimeException";

bool isFakeExceptionClass(const LoadedClass& lc)
{
    if (!lc.session) return false;
    const jbc::ir::ClassUnit& u = lc.session->unit();
    if (u.version != kGeneratedVersion) return false;
    // 0 字段 + 0 方法：假异常类只有类头，构造器都没有。
    if (!u.fields.empty() || !u.methods.empty()) return false;
    return u.symbols().str(u.superClass) == kRuntimeException;
}

// ── CFG：共用底座见 cfg.hpp；本 pass 只额外需要"含异常边"的可达性 ──

/// 从方法入口（下标 0）求可达指令集，异常边按 `cu.exc` 当前内容算。
///
/// 只在异常表**已经摘干净注入项**之后调用：注入 handler 一旦还在表里，它
/// 会把自己的 handler 体标成可达，死填充就删不掉。
///
/// 返回 false 表示"这个方法不碰"——标签解析失败或出现 jsr/ret。可达性判错
/// 的代价是删掉活指令（语义直接坏掉），所以任何不确定都放弃，不做猜测。
bool computeReachable(const CodeUnit& cu, std::vector<bool>& reach)
{
    const auto&     order = cu.dom.order();
    const ca::usize n     = order.size();
    reach.assign(n, false);
    if (n == 0) return true;

    const Layout lay = layoutOf(cu.dom);

    struct Edge
    {
        ca::usize start   = 0;
        ca::usize end     = 0;
        ca::usize handler = 0;
    };
    std::vector<Edge> edges;
    edges.reserve(cu.exc.size());
    for (const CodeUnit::Exc& e : cu.exc) {
        Edge x;
        if (!indexOfLabel(cu.dom, e.start, lay, x.start)) return false;
        if (!indexOfLabel(cu.dom, e.end, lay, x.end)) return false;
        if (!indexOfLabel(cu.dom, e.handler, lay, x.handler)) return false;
        edges.push_back(x);
    }

    std::vector<ca::usize> work;
    const auto mark = [&](ca::usize i) -> bool {
        if (i >= n || reach[i]) return false;
        reach[i] = true;
        work.push_back(i);
        return true;
    };

    mark(0);
    bool progress = true;
    while (progress) {
        progress = false;
        while (!work.empty()) {
            const ca::usize i  = work.back();
            work.pop_back();
            const Insn*     in = cu.dom.peek(order[i]);
            if (in == nullptr) return false;

            const Flow f = flowOf(in->op);
            if (f == Flow::Subroutine) return false;

            if (f == Flow::Fall || f == Flow::Cond) {
                progress = mark(i + 1) || progress;
            }
            if (f == Flow::Jump || f == Flow::Cond) {
                ca::usize t = 0;
                if (!indexOfLabel(cu.dom, in->target, lay, t)) return false;
                progress = mark(t) || progress;
            } else if (f == Flow::Switch) {
                ca::usize t = 0;
                if (!indexOfLabel(cu.dom, in->target, lay, t)) return false;
                progress = mark(t) || progress;
                for (const LabelId label : in->targets) {
                    if (!indexOfLabel(cu.dom, label, lay, t)) return false;
                    progress = mark(t) || progress;
                }
            }
        }
        // 异常边：try 区间内只要有一条可达指令，handler 就可达。表里此时
        // 只剩真 handler，所以这条边算出来的就是"原本就该可达"的集合。
        for (const Edge& e : edges) {
            if (reach[e.handler]) continue;
            for (ca::usize i = e.start; i < e.end && i < n; ++i) {
                if (!reach[i]) continue;
                progress = mark(e.handler) || progress;
                break;
            }
        }
    }
    return true;
}

bool containsLabel(const std::vector<LabelId>& v, LabelId l)
{
    for (const LabelId x : v) {
        if (x == l) return true;
    }
    return false;
}

/// 假异常类的 catch_type 名（解析不出来返回空）。
std::string_view catchName(jbc::ir::ClassSession& session, ca::u32 catchCp)
{
    if (catchCp == 0) return {};
    return session.backing().className(catchCp);
}

/// 按 URI 摘除资源（提交后资源已被替换，只能按 URI 找，不能拿旧指针）。
/// 指令里是否引用某个类（new/checkcast/anewarray/instanceof/multianewarray
/// 与 ldc 的 Class 常量）。用于删除假异常类前的最后一道引用检查。
bool insnReferencesClass(jbc::ir::ClassSession& session, const Insn& in, std::string_view name)
{
    if (session.classOf(in) == name) return true;
    jbc::ir::ClassSession::ConstSem sem;
    if (!session.constOf(in, sem)) return false;
    return sem.kind == jbc::ir::ClassSession::ConstSem::Kind::Class && sem.str == name;
}

}   // namespace

mjbase::spi::PreservedAnalyses DashoControlFlowPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    static_cast<void>(am);

    ClassSet set;
    set.load(container);
    if (set.classes().empty()) return mjbase::spi::PreservedAnalyses::all();

    // ── 1. 圈定假异常类 ──
    std::unordered_set<std::string> fake;
    for (const LoadedClass& lc : set.classes()) {
        if (isFakeExceptionClass(lc)) fake.emplace(lc.name);
    }
    if (fake.empty()) return mjbase::spi::PreservedAnalyses::all();

    ca::usize                       droppedEntries    = 0;
    ca::usize                       structuralMethods = 0;
    std::unordered_set<std::string> keepAlive;   // 仍被引用、不能删的假异常类

    for (LoadedClass& lc : set.classes()) {
        if (fake.count(lc.name) != 0) continue;

        const ca::usize n = lc.session->unit().methods.size();
        for (ca::usize i = 0; i < n; ++i) {
            if (!jbc::ir::hasCode(*lc.session, i)) continue;
            auto promoted = jbc::ir::promoteMethod(*lc.session, i);
            if (promoted.is_err()) continue;
            jbc::ir::CodeUnitRef ref = std::move(promoted).unwrap();
            CodeUnit&            cu  = *ref;

            // 指令级引用检查（new/checkcast/ldc Class）：删类前的安全网。
            for (const InsnId id : cu.dom.order()) {
                const Insn* in = cu.dom.peek(id);
                if (in == nullptr) continue;
                for (const std::string& name : fake) {
                    if (keepAlive.count(name) != 0) continue;
                    if (insnReferencesClass(*lc.session, *in, name)) keepAlive.emplace(name);
                }
            }

            if (cu.exc.empty()) continue;

            // ── 2. 按 catch_type 分流异常表项 ──
            std::vector<CodeUnit::Exc> survivors;
            std::vector<CodeUnit::Exc> dropped;
            survivors.reserve(cu.exc.size());
            for (const CodeUnit::Exc& e : cu.exc) {
                const std::string_view name = catchName(*lc.session, e.catchCp);
                if (!name.empty() && fake.count(std::string(name)) != 0) {
                    dropped.push_back(e);
                } else {
                    survivors.push_back(e);
                }
            }
            if (dropped.empty()) continue;

            // ── 3. 回收只被摘除项引用的边界标签 ──
            // 异常表标签是 Boundary 角色，会保护锚点；不先销毁，后面删 handler
            // 体会被 eraseBatch 以 StillUsed 整批拒绝。存活项还在用的标签必须
            // 留着（同一方法内多条注入项常共用同一个 handler 标签）。
            std::vector<LabelId> deadLabels;
            const auto           noteDead = [&](LabelId l) {
                for (const CodeUnit::Exc& s : survivors) {
                    if (s.start == l || s.end == l || s.handler == l) return;
                }
                if (!containsLabel(deadLabels, l)) deadLabels.push_back(l);
            };
            for (const CodeUnit::Exc& e : dropped) {
                noteDead(e.start);
                noteDead(e.end);
                noteDead(e.handler);
            }

            cu.exc = survivors;
            for (const LabelId l : deadLabels) {
                (void)cu.dom.destroyLabel(l);
            }

            // ── 4. 结构改写（异常表已空）vs 保布局改写 ──
            bool structural = survivors.empty();
            if (structural) {
                std::vector<bool> reach;
                if (!computeReachable(cu, reach)) {
                    structural = false;
                } else {
                    std::vector<InsnId> dead;
                    const auto&         order = cu.dom.order();
                    for (ca::usize k = 0; k < order.size(); ++k) {
                        if (!reach[k]) dead.push_back(order[k]);
                    }
                    if (!dead.empty()) {
                        jbc::ir::CodeEditor editor(cu);
                        if (editor.eraseBatch(dead,
                                              jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed)
                                .is_err())
                        {
                            structural = false;
                        }
                    }
                    if (structural) tidyGotoToNext(cu);
                }
            }

            if (!structural) {
                // 帧没重算：handler 帧里还写着假异常类，类不能删。
                for (const CodeUnit::Exc& e : dropped) {
                    const std::string_view name = catchName(*lc.session, e.catchCp);
                    if (!name.empty()) keepAlive.emplace(name);
                }
            }

            lc.dirty.push_back(DirtyMethod{i, structural});
            droppedEntries += dropped.size();
            if (structural) ++structuralMethods;
        }
    }

    if (droppedEntries == 0) return mjbase::spi::PreservedAnalyses::all();

    // ── 5. 写回 ──
    ca::usize dirtyClasses = 0;
    for (const LoadedClass& lc : set.classes()) {
        if (!lc.dirty.empty()) ++dirtyClasses;
    }
    const ca::usize written = set.commit(container);

    // ── 6. 删假异常类 ──
    // 只有"全部写回成功"才删：任一类写回失败就会留在原始字节上，而原始
    // 异常表仍引用这些类。另加 keepAlive：保布局改写的方法、或指令里显式
    // 引用过（new/checkcast）的类，一律保留。
    ca::usize deletedClasses = 0;
    if (written == dirtyClasses) {
        for (const LoadedClass& lc : set.classes()) {
            if (fake.count(lc.name) == 0) continue;
            if (keepAlive.count(lc.name) != 0) continue;
            if (eraseResource(container, lc.uri)) ++deletedClasses;
        }
    }

    CA_LOG_INFO("[dasho.ControlFlow] 摘除 {} 个注入异常表项（{} 个方法结构改写），"
                "删除 {} 个假异常类，写回 {} 个类",
                droppedEntries, structuralMethods, deletedClasses, written);
    return mjbase::spi::PreservedAnalyses::none();
}

Result<void, std::string> registerControlFlow(mjbase::spi::PassNameRegistry& reg)
{
    if (Result<void, mjbase::spi::RegistryError> r = reg.registerGlobalPass(
            "dasho.ControlFlow",
            [] { return std::make_unique<DashoControlFlowPass>(); },
            "Strip DashO controlflow tryCatch injection: drop exception-table entries whose"
            " catch_type is a generated fake exception class (classfile 45.3 + no members +"
            " RuntimeException), then prune now-unreachable fill/handler code when the"
            " table empties; layout-preserving when genuine handlers remain.");
        r.is_err()) {
        return Err(std::string("dasho.ControlFlow"));
    }
    return Ok();
}

}   // namespace dasho
