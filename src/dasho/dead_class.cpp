#include "dasho/dead_class.hpp"

#include <string>

#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/units.hpp"

namespace dasho {

using jbc::ir::ClassSession;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;

namespace {

/// 描述符里是否出现候选类名（候选只一个，子串查足够）。
bool descMentions(std::string_view desc, std::string_view candidate)
{
    return desc.find(candidate) != std::string_view::npos;
}

/// 类头 + 成员声明里的引用（不含方法体）。
bool headerMentions(ClassSession& session, std::string_view candidate)
{
    const jbc::ir::ClassUnit& u = session.unit();

    if (u.symbols().str(u.superClass) == candidate) return true;
    for (const jbc::ir::SymbolId itf : u.interfaces) {
        if (u.symbols().str(itf) == candidate) return true;
    }
    for (const jbc::ir::FieldUnit& f : u.fields) {
        if (descMentions(u.symbols().str(f.desc), candidate)) return true;
    }
    for (const jbc::ir::MethodUnit& m : u.methods) {
        if (descMentions(u.symbols().str(m.desc), candidate)) return true;
    }
    return false;
}

/// 一条指令是否引用候选类。
bool insnMentions(ClassSession& session, const Insn& in, std::string_view candidate)
{
    if (session.classOf(in) == candidate) return true;

    ClassSession::MemberSem sem;
    if (session.memberOf(in, sem) && sem.owner == candidate) return true;

    ClassSession::ConstSem csem;
    if (session.constOf(in, csem) && csem.kind == ClassSession::ConstSem::Kind::Class &&
        csem.str == candidate)
    {
        return true;
    }
    return false;
}

/// 方法体（指令 + 异常表）里是否引用候选类。
/// @param found 命中时置真。
/// @return 判定是否成功；false = 方法体解不开，调用方必须保守处理。
bool bodyMentions(ClassSession& session, ca::usize methodIdx, std::string_view candidate,
                  bool& found)
{
    found = false;
    if (!jbc::ir::hasCode(session, methodIdx)) return true;

    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) return false;

    jbc::ir::CodeUnitRef unit = std::move(promoted).unwrap();
    for (const InsnId id : unit->dom.order()) {
        const Insn* in = unit->dom.peek(id);
        if (in == nullptr) continue;
        if (insnMentions(session, *in, candidate)) {
            found = true;
            return true;
        }
    }
    for (const CodeUnit::Exc& e : unit->exc) {
        if (session.backing().className(e.catchCp) == candidate) {
            found = true;
            return true;
        }
    }
    return true;
}

}  // namespace

bool eraseResource(mjbase::model::ResourceContainer& container, std::string_view uri)
{
    auto& resources = container.getResources();
    for (auto it = resources.begin(); it != resources.end(); ++it) {
        if (it->first == uri) {
            resources.erase(it);
            return true;
        }
    }
    return false;
}

namespace {

/// 类名尾段是纯数字（`Owner$1`、`Owner$12`）。
bool hasAnonymousSuffix(std::string_view name)
{
    const std::size_t dollar = name.rfind('$');
    if (dollar == std::string_view::npos || dollar + 1 >= name.size()) return false;
    for (std::size_t i = dollar + 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

bool isStatic(ca::u32 access) noexcept
{
    return (access & 0x0008u) != 0;   // ACC_STATIC
}

}  // namespace

bool looksLikeStringDecryptor(const jbc::ir::ClassUnit& unit, std::string_view className)
{
    if (!hasAnonymousSuffix(className)) return false;
    if (!unit.fields.empty()) return false;
    if (unit.methods.empty()) return false;

    bool anyStringReturn = false;
    for (const jbc::ir::MethodUnit& m : unit.methods) {
        const std::string_view name = unit.symbols().str(m.name);
        if (name == "<init>" || name == "<clinit>") return false;
        if (!isStatic(m.access)) return false;
        const std::string_view desc  = unit.symbols().str(m.desc);
        const std::size_t      paren = desc.find(')');
        if (paren != std::string_view::npos && desc.substr(paren) == ")Ljava/lang/String;") {
            anyStringReturn = true;
        }
    }
    return anyStringReturn;
}

std::unordered_set<std::string> collectDecryptorClasses(ClassSet& set)
{
    std::unordered_set<std::string> out;
    for (const LoadedClass& lc : set.classes()) {
        if (looksLikeStringDecryptor(lc.session->unit(), lc.name)) out.insert(lc.name);
    }
    return out;
}

bool isCandidateLive(ClassSet& set, std::string_view candidate)
{
    if (candidate.empty()) return true;

    for (const LoadedClass& lc : set.classes()) {
        if (lc.name == candidate) continue;   // 自引用不构成"被使用"

        // ── 预筛：CP 里有没有这个类名 ──
        // 池是只追加的，命中只说明"可能引用过"，仍需定判。
        bool                        suspect = false;
        const jbc::core::ClassFile& cf      = lc.session->backing();
        for (ca::u32 i = 1; i < cf.cp.size() && !suspect; ++i) {
            if (cf.cp.at(i).tag != jbc::core::CpTag::Class) continue;
            const std::string_view name = cf.className(i);
            if (name.empty() || name == lc.name) continue;
            suspect = (name == candidate);
        }
        if (!suspect) continue;

        // ── 定判 ──
        if (headerMentions(*lc.session, candidate)) return true;

        const ca::usize methods = lc.session->unit().methods.size();
        for (ca::usize mi = 0; mi < methods; ++mi) {
            bool       found = false;
            const bool ok    = bodyMentions(*lc.session, mi, candidate, found);
            if (!ok) return true;   // 解不开就当活：宁可留残留，不可删活类
            if (found) return true;
        }
    }
    return false;
}

DeadClassSweep eraseUnreferencedClasses(mjbase::model::ResourceContainer&       container,
                                        ClassSet&                               set,
                                        const std::unordered_set<std::string>& candidates)
{
    DeadClassSweep sweep;
    if (candidates.empty()) return sweep;

    for (const LoadedClass& lc : set.classes()) {
        if (candidates.count(lc.name) == 0) continue;
        if (isCandidateLive(set, lc.name)) {
            ++sweep.keptReferenced;
            continue;
        }
        if (eraseResource(container, lc.uri)) {
            ++sweep.deleted;
        } else {
            ++sweep.keptMissing;
        }
    }
    return sweep;
}

}  // namespace dasho
