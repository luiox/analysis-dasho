#include "dasho/checks_pass.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
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

/// 超过这个指令数就不做区间试探：hammock 判定是"每个候选右端跑一次可达性"，
/// 大方法上会退化成 O(n²)。DashO 的检查注入只会落进普通业务方法，几万条
/// 指令的巨型方法本来就该人工看，交给调用方跳过更合适。
constexpr ca::usize kMaxInsnsForRegion = 8192;

/// 一个方法内允许连续剥离的段数上限（`where="BeginningAndEnd"` 会有两段，
/// 留足余量；超过即认为判据出了意外，停下来）。
constexpr ca::usize kMaxRegionsPerMethod = 8;

// ==== SCAN ====================================================================
// 识别 dasho.Checks 注入的运行时检查段。两层判据，缺一不可。
//
//   粗找  引用解析 + 区间试探（hammock 单入口单出口）        可钉形状，便宜
//   细找  模板形状（反射三连 + `(seed*k)%seed` 谓词 + 响应）
//         判据等级 **S3**（实现选择）——所以本仓判据改过三轮
//   拒绝  缺模板 / 有噪声无响应 / 区间结构不成立 / 区间超限
//
// 第二层钉的是 S3，是 `Era` 分支唯一合法落点：模板形状是 DashO 的**实现选择**，
// 不是它非如此不可的语义后果。版本一变就该改这一层，而不是把版本号塞进
// rewrite 或 proof。
//
// ── 引用解析 ──

struct MemberRef
{
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

bool memberRef(jbc::ir::ClassSession& session, const Insn& in, MemberRef& out)
{
    jbc::ir::ClassSession::MemberSem sem;
    if (!session.memberOf(in, sem)) return false;
    out.owner = sem.owner;
    out.name  = sem.name;
    out.desc  = sem.desc;
    return true;
}

bool isCallTo(jbc::ir::ClassSession& session,
              const Insn&            in,
              std::string_view       owner,
              std::string_view       name)
{
    if (in.op != Op::INVOKESTATIC) return false;
    MemberRef ref;
    if (!memberRef(session, in, ref)) return false;
    return ref.owner == owner && ref.name == name;
}

// ── 后继图 / 可达性 / 不可达剪除：共用底座见 cfg.hpp ──

/// [f, X) 是否构成 hammock：
///   - 区间内每个位置要么能从 f 到达、要么本来就全局不可达（死填充）；
///   - 区间内每条指令的后继要么在区间内、要么恰好是 X；
///   - 区间外没有任何边进入区间**内部** `(f, X)`。
///
/// 注意第三条规定的是 `(f, X)` 而不是 `[f, X)`：**允许外部边进入 f**。注入段
/// 在 `where="Beginning"` 时 f=0（没有入边），但 `where="End"` 时 DashO 会把原
/// 表达式的值溢写到局部变量、把注入段接在它后面，此时 f>0 且必然有一条从 f-1
/// 落下来的入口边。这条边删完之后会被 `kRetargetToFallthrough` 改锚到 X，语义
/// 不变（注入段对方法状态没有贡献）。
///
/// 唯一的例外是**回边**：源位置在区间之后（`i >= X`）却跳回 f，说明这段是循环
/// 体，删掉会把循环变成顺序流。按"源在区间之后就当回边"拒绝——Java 字节码的
/// 布局跟随结构，真回边的源必然在后面。
bool isHammock(const Graph&             g,
               const std::vector<bool>& alive,
               ca::usize                f,
               ca::usize                X,
               std::vector<bool>&       scratch)
{
    if (X <= f) return false;
    reachSet(g, f, X, scratch);
    for (ca::usize i = f; i < X; ++i) {
        if (!scratch[i] && alive[i]) return false;
    }
    for (ca::usize i = f; i < X; ++i) {
        for (const ca::usize v : g.succ[i]) {
            if (v != X && (v < f || v >= X)) return false;
        }
    }
    const ca::usize n = g.succ.size() - 1;
    for (ca::usize i = 0; i <= n; ++i) {
        if (i >= f && i < X) continue;
        for (const ca::usize v : g.succ[i]) {
            if (v > f && v < X) return false;   // 进入区间内部：拒绝
            if (v == f && i >= X) return false;   // 区间之后跳回 f：回边
        }
    }
    return true;
}

// ── 模板判据 ──

bool isIntConstPush(jbc::ir::ClassSession& session, const Insn& in)
{
    switch (in.op) {
    case Op::ICONST_M1:
    case Op::ICONST_0:
    case Op::ICONST_1:
    case Op::ICONST_2:
    case Op::ICONST_3:
    case Op::ICONST_4:
    case Op::ICONST_5:
    case Op::BIPUSH:
    case Op::SIPUSH:
        return true;
    case Op::LDC:
    case Op::LDC_W: {
        jbc::ir::ClassSession::ConstSem sem;
        return session.constOf(in, sem)
            && sem.kind == jbc::ir::ClassSession::ConstSem::Kind::Int;
    }
    default:
        return false;
    }
}

/// 反射三连：`Class.forName` / `Class.getMethod` / `Method.invoke`。
bool hasReflectionTriple(jbc::ir::ClassSession& session, const CodeUnit& cu, ca::usize f, ca::usize X)
{
    const auto& order = cu.dom.order();
    bool        forName = false;
    bool        getMethod = false;
    bool        invoke = false;
    for (ca::usize i = f; i < X; ++i) {
        const Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) continue;
        MemberRef ref;
        if (!memberRef(session, *in, ref)) continue;
        if (ref.owner == "java/lang/Class" && ref.name == "forName") forName = true;
        if (ref.owner == "java/lang/Class" && ref.name == "getMethod") getMethod = true;
        if (ref.owner == "java/lang/reflect/Method" && ref.name == "invoke") invoke = true;
    }
    return forName && getMethod && invoke;
}

/// 噪声谓词窗口：`invokestatic ()I` + `dup` + 整型常量 + `imul` + `swap` +
/// `irem` + 条件跳转。
///
/// DashO 的语义是 `(seed * k) % seed`：`seed` 由 `()I` 生成器给出，两次读取
/// 之间被 `dup` 固定成同一个值，所以恒为 0，`if<c>` 的选路是**确定的**。这
/// 七条是注入模板本身——换实现只会换常量与跳转方向，不会换这个形状；而真实
/// 业务代码不会平白写一条 `x = (f() * 4) % f()` 再拿结果分支。
bool hasNoisePredicate(jbc::ir::ClassSession& session, const CodeUnit& cu, ca::usize f, ca::usize X)
{
    const auto& order = cu.dom.order();
    const auto  at    = [&](ca::usize i) { return cu.dom.peek(order[i]); };

    for (ca::usize i = f; i + 6 < X; ++i) {
        const Insn* a = at(i);
        const Insn* b = at(i + 1);
        const Insn* c = at(i + 2);
        const Insn* d = at(i + 3);
        const Insn* e = at(i + 4);
        const Insn* s = at(i + 5);
        const Insn* j = at(i + 6);
        if (a == nullptr || b == nullptr || c == nullptr || d == nullptr || e == nullptr
            || s == nullptr || j == nullptr) {
            continue;
        }
        // 生成器的 owner 是 `Owner$N`（类名不可预知），只校验"静态无参返回 int"。
        if (a->op != Op::INVOKESTATIC) continue;
        MemberRef seed;
        if (!memberRef(session, *a, seed) || seed.desc != "()I") continue;
        if (b->op != Op::DUP) continue;
        if (!isIntConstPush(session, *c)) continue;
        if (d->op != Op::IMUL) continue;
        if (e->op != Op::SWAP) continue;
        if (s->op != Op::IREM) continue;
        if (flowOf(j->op) != Flow::Cond) continue;
        return true;
    }
    return false;
}

/// 响应：注入段必须能"做点什么"。没有响应就没有检查，DashO 自己也会把这种
/// 配置判成 unused——所以这条同时是"这是检查而不是普通反射代码"的判据。
bool hasResponse(jbc::ir::ClassSession& session, const CodeUnit& cu, ca::usize f, ca::usize X)
{
    const auto& order = cu.dom.order();
    for (ca::usize i = f; i < X; ++i) {
        const Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) continue;
        if (in->op == Op::ATHROW) return true;
        MemberRef ref;
        if (!memberRef(session, *in, ref)) continue;
        if ((ref.owner == "java/lang/System" || ref.owner == "java/lang/Runtime")
            && (ref.name == "exit" || ref.name == "halt")) {
            return true;
        }
        if (ref.owner == "java/lang/Thread" && ref.name == "sleep") return true;
    }
    return false;
}

// ── 区间检测 ──

struct Region
{
    ca::usize begin = 0;
    ca::usize end   = 0;
};

/// 找一个可剥离的注入段。
///
/// 入口候选 = "包含 `Class.forName` 的异常表项的起点"。注入段一定是整段被
/// `try { ... } catch (Exception) { ... }` 包住的，所以 try 的起点就是区间
/// 起点——不需要从锚点往前扫着猜。
bool findRegion(jbc::ir::ClassSession& session, const CodeUnit& cu, const Graph& g, Region& out)
{
    const auto&     order = cu.dom.order();
    const ca::usize n     = order.size();
    if (n == 0 || n > kMaxInsnsForRegion) return false;

    const Layout lay = layoutOf(cu.dom);

    std::vector<ca::usize> anchors;
    std::vector<ca::usize> rights;   // 候选右端：全部跳转落点 + 方法末尾
    for (ca::usize i = 0; i < n; ++i) {
        const Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) return false;
        if (isCallTo(session, *in, "java/lang/Class", "forName")) anchors.push_back(i);
        for (const ca::usize v : g.succ[i]) rights.push_back(v);
    }
    if (anchors.empty()) return false;
    rights.push_back(n);
    std::sort(rights.begin(), rights.end());
    rights.erase(std::unique(rights.begin(), rights.end()), rights.end());

    // 入口候选 = 锚点所在表项的 handler 分组后，组内最小的 from。
    //
    // 为什么按 handler 分组而不是直接取"锚点所在表项的 from"：控制流注入的块
    // 切分会把检查段那一条 try 拆成几十段，每段一个表项，但**共用同一个
    // handler**。只取锚点所在那一段的 from，会把区间起点算到方法中段，前面的
    // 检查码就留在原地（语料实测：块切分后同一方法的检查段可拆成几十段表项
    // 共享同一 handler，只取单段会让起点落到方法中段）。
    std::vector<ca::usize> handlers;
    for (const CodeUnit::Exc& e : cu.exc) {
        ca::usize s = 0, t = 0, h = 0;
        if (!indexOfLabel(cu.dom, e.start, lay, s)) return false;
        if (!indexOfLabel(cu.dom, e.end, lay, t)) return false;
        if (!indexOfLabel(cu.dom, e.handler, lay, h)) return false;
        for (const ca::usize a : anchors) {
            if (s <= a && a < t) {
                if (std::find(handlers.begin(), handlers.end(), h) == handlers.end()) {
                    handlers.push_back(h);
                }
                break;
            }
        }
    }
    if (handlers.empty()) return false;

    std::vector<ca::usize> starts;
    for (const ca::usize h : handlers) {
        bool      has = false;
        ca::usize lo  = 0;
        for (const CodeUnit::Exc& e : cu.exc) {
            ca::usize s = 0, t = 0, h2 = 0;
            if (!indexOfLabel(cu.dom, e.start, lay, s)) return false;
            if (!indexOfLabel(cu.dom, e.end, lay, t)) return false;
            if (!indexOfLabel(cu.dom, e.handler, lay, h2)) return false;
            static_cast<void>(t);
            if (h2 != h) continue;
            if (!has || s < lo) lo = s;
            has = true;
        }
        if (has) starts.push_back(lo);
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    std::vector<bool> alive;
    computeAlive(g, handlers, alive);

    std::vector<bool> scratch;
    for (const ca::usize f : starts) {
        for (const ca::usize X : rights) {
            if (X <= f) continue;
            // 整个方法都是检查段：删完方法就空了，没有"直通"可留，放弃。
            if (f == 0 && X == n) continue;
            if (!isHammock(g, alive, f, X, scratch)) continue;
            // 必须覆盖方法内**全部** forName 锚点。块切分会把一条注入段切成多个
            // 首尾相接的 hammock，只按"最小的合法右端"会把后半段留在原地。
            bool coversAll = true;
            for (const ca::usize a : anchors) {
                if (!(f <= a && a < X)) {
                    coversAll = false;
                    break;
                }
            }
            if (!coversAll) continue;
            if (!hasReflectionTriple(session, cu, f, X)) continue;
            if (!hasNoisePredicate(session, cu, f, X)) continue;
            if (!hasResponse(session, cu, f, X)) continue;
            out.begin = f;
            out.end   = X;
            return true;
        }
    }
    return false;
}

// ==== REWRITE =================================================================
// 只消费 SCAN 段给出的 Region，不回头重新找站点（CONVENTION.md §3.2 第 4 条）。
//
//   stripOne  **先摘异常表、再删指令** —— 顺序不可换，见函数注释
//
// 改写策略选的是"结构剥离"（整段删除），与 control_flow 的"保布局"相对。能不能
// 这么做取决于摘完之后帧还能不能重算——那是语义事实，与版本无关，所以本段禁止
// 出现任何版本分支。
//
// ── 剥离 ──

/// 把 [f, X) 整段删掉，并摘掉整条落在区间内的异常表项。
///
/// 顺序很关键：**先改异常表，再删指令**。`eraseBatch` 把异常表边界引用当
/// `StillUsed` 拒绝（改边界等于改保护语义），所以边界项必须先离开表；而删完
/// 指令之后标签句柄已经失效，再想按内容匹配表项就匹配不上了（实测：先删指令
/// 会留下一条指向已删 pc 的陈旧表项，类能导出但校验器直接拒）。
bool stripOne(CodeUnit& cu, const Region& r)
{
    const auto&  order = cu.dom.order();
    const Layout lay   = layoutOf(cu.dom);

    std::vector<CodeUnit::Exc> survivors;
    std::vector<CodeUnit::Exc> dropped;
    survivors.reserve(cu.exc.size());
    for (const CodeUnit::Exc& e : cu.exc) {
        ca::usize s = 0, t = 0, h = 0;
        if (!indexOfLabel(cu.dom, e.start, lay, s)) return false;
        if (!indexOfLabel(cu.dom, e.end, lay, t)) return false;
        if (!indexOfLabel(cu.dom, e.handler, lay, h)) return false;

        const bool whole = (s >= r.begin && t <= r.end && h >= r.begin && h < r.end);
        if (whole) {
            dropped.push_back(e);
            continue;
        }
        const bool touches = (s < r.end && t > r.begin) || (h >= r.begin && h < r.end);
        if (touches) return false;   // 部分重叠：改边界会改保护语义，放弃
        survivors.push_back(e);
    }
    // 一条都不摘说明判据和异常表对不上：宁可不动，也不要留下"代码删了、
    // 表项还在"的半成品。
    if (dropped.empty()) return false;

    std::vector<InsnId> victims;
    victims.reserve(r.end - r.begin);
    for (ca::usize i = r.begin; i < r.end; ++i) victims.push_back(order[i]);

    // 只被摘除项引用的边界标签：必须**在删除指令之前**销毁。异常表标签是
    // Boundary 角色，会保护自己的锚点；不先销毁，eraseBatch 会把整个批次按
    // StillUsed 拒绝（实测 err=2）。存活项还在用的标签要留着——同一方法里
    // 多条表项常共用同一个 handler 标签。
    std::vector<LabelId> dead;
    const auto           noteDead = [&](LabelId l) {
        for (const CodeUnit::Exc& s : survivors) {
            if (s.start == l || s.end == l || s.handler == l) return;
        }
        for (const LabelId x : dead) {
            if (x == l) return;
        }
        dead.push_back(l);
    };
    for (const CodeUnit::Exc& e : dropped) {
        noteDead(e.start);
        noteDead(e.end);
        noteDead(e.handler);
    }

    cu.exc = survivors;
    for (const LabelId l : dead) {
        (void)cu.dom.destroyLabel(l);
    }

    jbc::ir::CodeEditor editor(cu);
    if (editor.eraseBatch(victims, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
            .is_err()) {
        return false;
    }

    if (cu.exc.empty()) tidyGotoToNext(cu);
    return true;
}

// ==== SCAN（次级残留：死类候选）==============================================
// checkRuntimeFacade 是注入进来的工具类；上面的检查段被摘掉之后它就没人引用了。
// 判据等级 **S3**：钉的是命名前缀 + 空类形状，属于实现选择，跨版本必须复核。
//
// ── 死类清扫的候选 ──

/// 检查运行时门面形态：`Bootstrap` 那一类。
///
///   - 至少一个 `native` 方法；
///   - 除 `<init>` 外全部 `static`；
///   - 没有实例字段。
///
/// 真 JNI 类通常有实例 native 方法，所以这个判据不会把它们卷进来；而
/// DashO 的检查门面是纯静态 + 构造器私有，正好落在这里。候选只是"允许考虑"，
/// 真正删除还要过 `eraseUnreferencedClasses` 的活引用检查。
bool looksLikeCheckRuntimeFacade(const jbc::ir::ClassUnit& unit)
{
    bool anyNative = false;
    for (const jbc::ir::MethodUnit& m : unit.methods) {
        const std::string_view name = unit.symbols().str(m.name);
        if (name == "<init>") continue;
        if ((m.access & 0x0008u) == 0) return false;   // ACC_STATIC
        if ((m.access & 0x0100u) != 0) anyNative = true;   // ACC_NATIVE
    }
    if (!anyNative) return false;
    for (const jbc::ir::FieldUnit& f : unit.fields) {
        if ((f.access & 0x0008u) == 0) return false;   // 实例字段
    }
    return true;
}

std::unordered_set<std::string> collectCheckRuntimeFacades(ClassSet& set)
{
    std::unordered_set<std::string> out;
    for (const LoadedClass& lc : set.classes()) {
        if (looksLikeCheckRuntimeFacade(lc.session->unit())) out.insert(lc.name);
    }
    return out;
}

}   // namespace

// ==== PROOF ===================================================================
// 判定 + 回滚。run() 只做编排。
//
//   类级门  ClassSet::commit —— 帧再生 + 校验器 + 类数不增，失败回滚该类
//   死类门  只删"零引用且自己就是 facade"的类；有活引用就留着（fail-closed）
//   包级门  整包不发布
//
// 回滚粒度是**类**。禁止"修一下让它过"。
mjbase::spi::PreservedAnalyses DashoChecksPass::run(mjbase::model::ResourceContainer& container,
                                                    mjbase::spi::GlobalAnalysisManager& am)
{
    static_cast<void>(am);

    ClassSet set;
    set.load(container);
    if (set.classes().empty()) return mjbase::spi::PreservedAnalyses::all();

    ca::usize                       strippedMethods = 0;
    ca::usize                       dirtyClasses    = 0;
    bool                            aborted         = false;
    std::unordered_set<std::string> touched;   // 注入段里出现过的类（删类候选）

    for (LoadedClass& lc : set.classes()) {
        const ca::usize n = lc.session->unit().methods.size();
        for (ca::usize i = 0; i < n && !aborted; ++i) {
            if (!jbc::ir::hasCode(*lc.session, i)) continue;
            auto promoted = jbc::ir::promoteMethod(*lc.session, i);
            if (promoted.is_err()) continue;
            jbc::ir::CodeUnitRef ref = std::move(promoted).unwrap();
            CodeUnit&            cu  = *ref;

            ca::usize done = 0;
            for (ca::usize round = 0; round < kMaxRegionsPerMethod; ++round) {
                Graph g;
                if (!buildGraph(cu, g)) break;
                Region r;
                if (!findRegion(*lc.session, cu, g, r)) break;

                // 先把区间里出现过的类记下来——删完就再也读不到了。
                const auto& order = cu.dom.order();
                for (ca::usize k = r.begin; k < r.end; ++k) {
                    const Insn* in = cu.dom.peek(order[k]);
                    if (in == nullptr) continue;
                    const std::string_view cls = lc.session->classOf(*in);
                    if (!cls.empty()) touched.emplace(cls.data(), cls.size());
                    MemberRef ref2;
                    if (memberRef(*lc.session, *in, ref2) && !ref2.owner.empty()) {
                        touched.emplace(ref2.owner.data(), ref2.owner.size());
                    }
                }

                if (!stripOne(cu, r)) {
                    // 判据说这里是注入段，改写却没做成：方法体已不可信。
                    // 本 pass 只在 set.commit 里落盘，所以直接放弃整趟，
                    // 容器保持原样——不做"改一半"的输出。
                    CA_LOG_WARN("[dasho.Checks] {}#{} 注入段剥离失败（区间 [{}, {})）",
                                lc.name, i, r.begin, r.end);
                    aborted = true;
                    break;
                }
                ++done;
            }
            if (done > 0) {
                // 摘完之后顺手把死填充清掉：不清的话，帧再生不会给死块发帧，
                // 产物过不了校验（`VerifyError: Expecting a stack map frame`）。
                pruneUnreachable(cu);
                tidyGotoToNext(cu);
                // `where="End"` 会留下 `xSTORE n; xLOAD n` 溢写对，一并收掉。
                foldStoreLoadPairs(cu);
                lc.dirty.push_back(DirtyMethod{i, true});
                strippedMethods += done;
            }
        }
        if (aborted) break;
        if (!lc.dirty.empty()) ++dirtyClasses;
    }

    if (aborted) return mjbase::spi::PreservedAnalyses::all();

    if (strippedMethods == 0) {
        CA_LOG_INFO("[dasho.Checks] 未发现可剥离的检查注入段");
        return mjbase::spi::PreservedAnalyses::all();
    }

    const ca::usize written = set.commit(container);

    // ── 死类清扫 ──
    // 候选三路并集：
    //   1. 注入段里出现过的类（含被 `new`/`invokestatic` 直接引用的 helper）；
    //   2. 形态上就是注入 helper 的 `Owner$<数字>` 壳（补 1 的漏：DashO 会为
    //      入口闭包里的每个类预生成一份 helper，很多从未被注入段调用过）；
    //   3. 检查运行时门面（全 static native + 无实例字段）。
    // 三路都只是"允许考虑"，真正删除由活引用检查兜底。
    DeadClassSweep sweep;
    if (written == dirtyClasses) {
        std::unordered_set<std::string> candidates = collectDecryptorClasses(set);
        candidates.insert(touched.begin(), touched.end());
        for (const std::string& f : collectCheckRuntimeFacades(set)) candidates.insert(f);
        sweep = eraseUnreferencedClasses(container, set, candidates);
    }

    CA_LOG_INFO("[dasho.Checks] 剥离 {} 个检查注入段，写回 {} 个类，"
                "删除 {} 个残留类（保留有引用 {} 个）",
                strippedMethods, written, sweep.deleted, sweep.keptReferenced);
    return mjbase::spi::PreservedAnalyses::none();
}

Result<void, std::string> registerChecks(mjbase::spi::PassNameRegistry& reg)
{
    if (Result<void, mjbase::spi::RegistryError> r = reg.registerGlobalPass(
            "dasho.Checks",
            [] { return std::make_unique<DashoChecksPass>(); },
            "Strip DashO runtime check injection: locate the injected segment as a single-entry"
            " single-exit hammock containing the reflection triple (Class.forName/getMethod/"
            "Method.invoke), the (seed*k)%seed noise predicate and a response (exit/halt/sleep/"
            " athrow), then delete the whole segment plus its exception-table entries; dead"
            " helper classes are swept afterwards.");
        r.is_err()) {
        return Err(std::string("dasho.Checks"));
    }
    return Ok();
}

}   // namespace dasho
