#include "dasho/restore_plan.hpp"

#include <unordered_map>

#include "jbc/type/descriptor.hpp"

namespace dasho {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

/// 未命中类表时返回输入本身（描述符里的 JDK 类型、包外类型都不在 map 里）。
std::string_view mapOrKeep(const RenameMap& map, std::string_view name)
{
    const std::string_view mapped = map.classNewName(name);
    return mapped.empty() ? name : mapped;
}

std::string uriOf(std::string_view internalName)
{
    std::string uri(internalName);
    uri += ".class";
    return uri;
}

}  // namespace

Result<std::string, std::string>
rewriteDescriptor(std::string_view desc,
                  const std::function<std::string_view(std::string_view)>& mapName)
{
    const auto spans = jbc::type::classNameSpans(desc);
    if (spans.is_err()) {
        return Err(std::string("描述符语法非法：") + std::string(desc));
    }

    std::string out;
    out.reserve(desc.size() + 16);
    ca::u32 cursor = 0;
    for (const jbc::type::ClassNameSpan& span : spans.unwrap()) {
        out.append(desc.data() + cursor, span.offset - cursor);
        out.append(mapName(desc.substr(span.offset, span.length)));
        cursor = span.offset + span.length;
    }
    out.append(desc.data() + cursor, desc.size() - cursor);
    return Ok(std::move(out));
}

RestorePlanStats buildRestorePlan(const RenameMap&                       map,
                                  mjbase::asm_remapper::MemberRemapPlan& plan,
                                  const RestorePlanOptions&              options)
{
    RestorePlanStats stats;

    // 第一段：类映射。mapfile 是 原名→混淆名，计划要 混淆名→原名。
    for (const auto& [original, obfuscated] : map.classes) {
        if (original == obfuscated) continue;
        if (options.skipObfClasses.count(obfuscated) != 0) {
            ++stats.uriConflict;
            continue;
        }
        plan.mapClass(obfuscated, original);
        ++stats.classRules;
    }

    // 第二段：成员规则。源键在混淆空间，目标键在原名空间——两边都要预映像。
    for (const MemberRename& member : map.members) {
        if (member.oldName == member.newName) {
            ++stats.identitySkipped;
            continue;
        }

        const std::string_view obfOwner = map.classNewName(member.owner);
        if (obfOwner.empty()) {
            // 类表里没有这个 owner：既不知道它在混淆后叫什么，也就无法定位。
            ++stats.unmappedOwner;
            continue;
        }
        // 注意：skip 表按**混淆名**记，这里必须用 obfOwner 判，不能用原名。
        if (options.skipObfClasses.count(std::string(obfOwner)) != 0) continue;

        const auto obfDesc = rewriteDescriptor(
            member.desc, [&map](std::string_view name) { return mapOrKeep(map, name); });
        if (obfDesc.is_err()) {
            ++stats.badDescriptor;
            continue;
        }

        if (member.isMethod) {
            plan.mapMethod(obfOwner, member.newName, obfDesc.unwrap(), member.owner, member.oldName,
                           member.desc);
            ++stats.methodRules;
        } else {
            plan.mapField(obfOwner, member.newName, obfDesc.unwrap(), member.owner, member.oldName,
                          member.desc);
            ++stats.fieldRules;
        }
    }

    return stats;
}

std::unordered_set<std::string>
findClassUriConflicts(const RenameMap& map, const std::vector<std::string>& uris)
{
    std::unordered_set<std::string> present;
    present.reserve(uris.size());
    for (const std::string& uri : uris) present.insert(uri);

    // 会被改走的混淆名：它们的 URI 腾得出来，不算占用。
    std::unordered_set<std::string> vacated;
    for (const auto& [original, obfuscated] : map.classes) {
        if (original != obfuscated) vacated.insert(obfuscated);
    }

    std::unordered_set<std::string> conflicts;
    for (const auto& [original, obfuscated] : map.classes) {
        if (original == obfuscated) continue;
        const std::string targetUri = uriOf(original);
        if (targetUri == uriOf(obfuscated)) continue;      // 原地不动
        if (present.count(targetUri) == 0) continue;        // 目标空着
        if (vacated.count(original) != 0) continue;         // 占用者自己也会改名
        conflicts.insert(obfuscated);
    }
    return conflicts;
}

}  // namespace dasho
