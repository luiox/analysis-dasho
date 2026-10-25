#include "dasho/class_set.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

#include "dasho/jvm_interp.hpp"

namespace dasho {

using ca::core::Ok;
using jbc::core::Op;
using jbc::ir::ClassSession;

namespace {

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccNative       = 0x0100;
constexpr ca::u32 kAccAbstract     = 0x0400;
constexpr ca::u32 kAccSynchronized = 0x0020;

/// 会话内按 name+desc 定位方法；未命中返回 kNoMethod。
ca::usize findMethod(ClassSession& session, std::string_view name, std::string_view desc)
{
    const auto& methods = session.unit().methods;
    for (ca::usize i = 0; i < methods.size(); ++i) {
        if (session.unit().symbols().str(methods[i].name) == name &&
            session.unit().symbols().str(methods[i].desc) == desc)
        {
            return i;
        }
    }
    return jbc::ir::kNoMethod;
}

// ── 帧再生（A1 链的本地实现）──

ca::u32 findOrAddUtf8(jbc::core::ClassFile& cf, std::string_view name)
{
    const ca::u32 found = cf.cp.findUtf8(name);
    return found != 0 ? found : cf.cp.addUtf8(name);
}

jbc::core::AttrSlot* codeSlotOf(jbc::core::ClassFile& cf, const jbc::ir::MethodUnit& mu)
{
    for (jbc::core::AttrSlot& slot : cf.methods[mu.backingIndex].attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, "Code")) return &slot;
    }
    return nullptr;
}

/// Strict 重算 → maxes 采用 → lower → SMT 再生挂回。
///
/// 只在**指令增删**（整段折叠）后用：那时既有帧的目标已被删除，必须按新
/// 布局重算。原地改指令（partial 改写）不走这里——布局与栈形都没变，原帧
/// 的 relocation 结果仍然有效，重算反而会引入与 verifier 保守合并不一致的
/// handler 帧（Strict 域的异常边只从可抛指令发出，比 verifier 精确）。
bool regenerateFrames(ClassSession& session, const mjbase::spi::MethodScope& scope,
                      jbc::analysis::ITypeResolver& resolver)
{
    namespace ja = jbc::analysis;
    if (session.backing().major < 50) return true;

    jbc::ir::CodeUnit&         cu   = scope.code;
    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::MethodUnit* mu   = unit.method(scope.method);
    if (mu == nullptr) return false;

    ja::MethodSig sig;
    sig.isStatic  = (mu->access & kAccStatic) != 0;
    sig.desc      = unit.symbols().str(mu->desc);
    sig.name      = unit.symbols().str(mu->name);
    sig.thisClass = unit.symbols().str(unit.thisClass);

    ja::FrameEngine          engine(session.backing().cp, resolver, ja::FramePolicy::Strict);
    std::vector<ja::FrameAt> frames;
    const ja::FrameSummary   summary = engine.compute(cu, sig, frames);
    if (!summary.ok) return false;

    if (!ja::applyComputedMaxes(cu, summary, sig)) return false;
    if (jbc::ir::lowerMethod(session, scope.method).is_err()) return false;

    auto built = ja::buildStackMapTable(session, cu, frames);
    if (built.is_err()) return false;
    auto result = std::move(built).unwrap();
    if (!result.needed) return true;

    jbc::core::ClassFile& cf       = session.backing();
    jbc::core::AttrSlot*  codeSlot = codeSlotOf(cf, *mu);
    if (codeSlot == nullptr) return false;
    const auto decoded = cf.decodeAttr(*codeSlot);
    if (decoded.is_err()) return false;
    auto* codeAttr = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
    auto& attrs    = codeAttr->attrs;
    attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                               [&](const jbc::core::AttrSlot& s) {
                                   return cf.cp.utf8Equals(s.nameCp, "StackMapTable");
                               }),
                attrs.end());
    attrs.push_back(jbc::core::AttrSlot::makeNew(
        findOrAddUtf8(cf, "StackMapTable"),
        std::unique_ptr<jbc::core::AttrBody>(std::move(result.attr))));
    return true;
}

}   // namespace

void ClassSet::load(mjbase::model::ResourceContainer& container)
{
    classes_.clear();
    byName_.clear();
    purity_.clear();

    for (auto& [uri, resource] : container.getResources()) {
        if (!resource || resource->getType() != mjbase::model::ResourceType::Class) continue;
        auto* cls = static_cast<mjbase::model::ClassResource*>(resource.get());
        const auto& bytes = cls->get();
        if (bytes.empty()) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
        if (sessionResult.is_err()) continue;
        LoadedClass entry;
        entry.session = std::move(sessionResult).unwrap();
        entry.name.assign(entry.session->backing().thisName());
        if (entry.name.empty()) continue;
        entry.resource = cls;
        entry.uri.assign(uri);
        byName_[entry.name] = entry.session.get();
        classes_.push_back(std::move(entry));
    }
}

ClassSession* ClassSet::find(std::string_view internalName) const
{
    const auto it = byName_.find(std::string(internalName));
    return it == byName_.end() ? nullptr : it->second;
}

bool ClassSet::isPure(ClassSession& session, ca::usize methodIdx)
{
    auto& slot = purity_[&session][methodIdx];
    if (slot == Purity::Unknown) {
        slot = computePurity(session, methodIdx) ? Purity::Pure : Purity::Impure;
    }
    return slot == Purity::Pure;
}

bool ClassSet::computePurity(ClassSession& session, ca::usize methodIdx)
{
    if (methodIdx >= session.unit().methods.size()) return false;
    if (!jbc::ir::hasCode(session, methodIdx)) return false;

    const jbc::ir::MethodUnit& mu = session.unit().methods[methodIdx];
    if ((mu.access & (kAccNative | kAccAbstract | kAccSynchronized)) != 0) return false;

    // 先置 InProgress：递归回到自身即判不纯（互递归环同样被截断）。
    auto& slot = purity_[&session][methodIdx];
    if (slot == Purity::InProgress) return false;
    slot = Purity::InProgress;

    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) {
        slot = Purity::Impure;
        return false;
    }
    jbc::ir::CodeUnitRef unit = std::move(promoted).unwrap();

    bool pure = true;
    for (const jbc::ir::InsnId id : unit->dom.order()) {
        const jbc::ir::Insn* in = unit->dom.peek(id);
        if (in == nullptr) {
            pure = false;
            break;
        }
        switch (in->op) {
        case Op::GETSTATIC: case Op::PUTSTATIC: case Op::GETFIELD: case Op::PUTFIELD:
        case Op::MONITORENTER: case Op::MONITOREXIT:
        case Op::ATHROW: case Op::INVOKEDYNAMIC: case Op::JSR: case Op::JSR_W:
        case Op::RET:
            pure = false;
            break;
        case Op::INVOKEVIRTUAL: case Op::INVOKESPECIAL: case Op::INVOKESTATIC:
        case Op::INVOKEINTERFACE: {
            ClassSession::MemberSem sem;
            if (!session.memberOf(*in, sem)) {
                pure = false;
                break;
            }
            if (isIntrinsicCall(sem.owner, sem.name, sem.desc)) break;
            ClassSession* target = find(sem.owner);
            if (target == nullptr) {
                pure = false;
                break;
            }
            const ca::usize idx = findMethod(*target, sem.name, sem.desc);
            if (idx == jbc::ir::kNoMethod || !isPure(*target, idx)) pure = false;
            break;
        }
        default:
            break;
        }
        if (!pure) break;
    }

    slot = pure ? Purity::Pure : Purity::Impure;
    return pure;
}

ca::usize ClassSet::commit(mjbase::model::ResourceContainer& container)
{
    // 帧链需要类层次图：本 jar 的类全部登记，JDK 类缺席时按 Object 回退
    // （Strict 合并的 fallback 语义，见 CommonSuperClassResolver）。
    mjbase::KeyPool                   pool;
    mjbase::hierarchy::ClassHierarchy hierarchy(pool);
    for (const LoadedClass& lc : classes_) {
        if (!lc.session) continue;
        const auto summary = mjbase::hierarchy::fillSummary(*lc.session, pool);
        (void)hierarchy.addClass(summary);
    }
    mjbase::spi::CommonSuperClassResolver superResolver(hierarchy);
    mjbase::spi::StrictTypeResolverAdapter typeResolver(superResolver);

    ca::usize written = 0;
    for (LoadedClass& lc : classes_) {
        if (lc.dirty.empty() || !lc.session) continue;

        bool ok = true;
        for (const DirtyMethod& dm : lc.dirty) {
            jbc::ir::MethodUnit& mu = lc.session->unit().methods[dm.idx];
            if (!mu.code) {
                ok = false;
                break;
            }
            // lower 前把嵌套属性（LNT/LVT/SMT）适配到可重定位形态；SMT 悬垂
            // 时整体摘除，由 lower 后的帧链再生。
            if (dm.structural) {
                (void)mjbase::spi::frame_rewire::prepareNestedAttrsForLayout(*lc.session,
                                                                            *mu.code);
            }
            if (jbc::ir::lowerMethod(*lc.session, dm.idx).is_err()) {
                CA_LOG_WARN("[dasho] lower 失败 {}#{} — 该类保持原样", lc.name, dm.idx);
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        for (const DirtyMethod& dm : lc.dirty) {
            if (!dm.structural) continue;
            jbc::ir::MethodUnit& mu = lc.session->unit().methods[dm.idx];
            if (!mu.code) continue;
            mjbase::spi::MethodTarget target{mjbase::MemberKey{},
                                             mu.id,
                                             lc.session->unit().symbols().str(mu.name),
                                             lc.session->unit().symbols().str(mu.desc),
                                             mu.access,
                                             *lc.session};
            mjbase::spi::MethodScope scope{target, *mu.code};
            if (!regenerateFrames(*lc.session, scope, typeResolver)) {
                CA_LOG_WARN("[dasho] 帧重算失败 {}.{} — 该类保持原样", lc.name,
                            std::string(lc.session->unit().symbols().str(mu.name)));
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        auto bytes = jbc::core::writeClass(lc.session->backing());
        if (bytes.is_err()) {
            CA_LOG_WARN("[dasho] writeClass 失败 {} — 该类保持原样", lc.name);
            continue;
        }
        container.replaceResource(
            lc.uri, std::make_unique<mjbase::model::ClassResource>(lc.uri,
                                                                  std::move(bytes).unwrap()));
        ++written;
    }
    return written;
}

}   // namespace dasho
