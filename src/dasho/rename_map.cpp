#include "dasho/rename_map.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dasho {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

/// 内部名规范化:mapfile 里可能写 `com.foo.Bar`(点分)也可能写 `com/foo/Bar`。
/// JVMS 4.2.1 禁止类名含 `.`、`;`、`[`、`/`,所以点转斜杠是安全且必要的归一。
std::string normalizeName(std::string_view s)
{
    std::string out(s);
    std::replace(out.begin(), out.end(), '.', '/');
    return out;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::vector<std::string_view> split(std::string_view s, char sep)
{
    std::vector<std::string_view> parts;
    ca::usize                     begin = 0;
    for (ca::usize i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            parts.push_back(s.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    return parts;
}

/// 按行切分(处理 CRLF 与末行无换行)。
std::vector<std::string_view> lines(std::string_view text)
{
    std::vector<std::string_view> out;
    ca::usize                     begin = 0;
    for (ca::usize i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            out.push_back(text.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    return out;
}

std::string joinKey(std::string_view owner, std::string_view name, std::string_view desc)
{
    std::string key;
    key.reserve(owner.size() + name.size() + desc.size() + 2);
    key.append(owner).push_back('\x1f');
    key.append(name).push_back('\x1f');
    key.append(desc);
    return key;
}

}   // namespace

void RenameMap::buildIndex()
{
    memberIndex_.clear();
    memberIndex_.reserve(members.size() * 2);
    for (ca::usize i = 0; i < members.size(); ++i) {
        const MemberRename& m = members[i];
        memberIndex_.emplace(joinKey(m.owner, m.oldName, m.desc), i);
    }
}

std::string_view RenameMap::memberNewName(std::string_view owner, std::string_view name,
                                          std::string_view desc) const
{
    if (memberIndex_.empty() && !members.empty()) {
        // 索引未建:退回线性扫描(小 map 或调用方忘了 buildIndex)。
        for (const MemberRename& m : members) {
            if (m.owner == owner && m.oldName == name && m.desc == desc) return m.newName;
        }
        return {};
    }
    const auto it = memberIndex_.find(joinKey(owner, name, desc));
    return it == memberIndex_.end() ? std::string_view{} : std::string_view(members[it->second].newName);
}

std::string_view RenameMap::classNewName(std::string_view oldName) const
{
    const auto it = classes.find(std::string(oldName));
    return it == classes.end() ? std::string_view{} : std::string_view(it->second);
}

// ── Java 风格类型 → JVM 描述符 ──

std::string javaTypeToDesc(std::string_view type)
{
    std::string_view t = trim(type);

    // 数组:尾部 `[]` 逐个折成前缀 `[`(多维写 `int[][]`)。
    ca::usize dims = 0;
    while (t.size() >= 2 && t.substr(t.size() - 2) == "[]") {
        t.remove_suffix(2);
        ++dims;
    }
    t = trim(t);

    std::string base;
    if (t == "void") {
        base = "V";
    } else if (t == "int") {
        base = "I";
    } else if (t == "long") {
        base = "J";
    } else if (t == "boolean") {
        base = "Z";
    } else if (t == "byte") {
        base = "B";
    } else if (t == "char") {
        base = "C";
    } else if (t == "short") {
        base = "S";
    } else if (t == "float") {
        base = "F";
    } else if (t == "double") {
        base = "D";
    } else if (!t.empty() && t.front() == 'L' && t.back() == ';') {
        base = std::string(t);   // 已经是描述符
    } else if (!t.empty() && t.front() == '[') {
        base = std::string(t);   // 已经是数组描述符
    } else {
        base = "L" + normalizeName(t) + ";";
    }

    if (dims == 0) return base;
    return std::string(dims, '[') + base;
}

// ── DashO 私有 .map ──

Result<RenameMap, std::string> parseDashoMap(std::string_view text)
{
    RenameMap  map;
    std::string current;   ///< 当前段头声明的旧类名

    for (std::string_view raw : lines(text)) {
        const std::string_view line = trim(raw);
        if (line.empty()) continue;

        if (line.front() == '|') {
            // |Old|New|
            const auto parts = split(line, '|');
            if (parts.size() != 4 || !parts.front().empty() || !parts.back().empty()) {
                ++map.skippedLines;
                continue;
            }
            const std::string oldName = normalizeName(parts[1]);
            const std::string newName = normalizeName(parts[2]);
            if (oldName.empty() || newName.empty()) {
                ++map.skippedLines;
                continue;
            }
            current = oldName;
            map.classes[oldName] = newName;
            if (oldName == newName) ++map.unchangedClasses;
            continue;
        }

        if (line.front() == '=' || line.front() == '+') {
            if (current.empty()) {
                ++map.skippedLines;   // 成员行出现在任何段头之前
                continue;
            }
            const bool isMethod = line.front() == '=';
            const auto parts    = split(line, line.front());
            if (parts.size() != 5 || !parts.front().empty() || !parts.back().empty()) {
                ++map.skippedLines;
                continue;
            }
            MemberRename m;
            m.owner    = current;
            m.oldName  = std::string(parts[1]);
            m.desc     = std::string(parts[2]);
            m.newName  = std::string(parts[3]);
            m.isMethod = isMethod;
            if (m.oldName.empty() || m.newName.empty() || m.desc.empty()) {
                ++map.skippedLines;
                continue;
            }
            if (m.oldName == m.newName) ++map.unchangedMembers;
            map.members.push_back(std::move(m));
            continue;
        }

        ++map.skippedLines;   // 注释 / 未知行
    }

    map.buildIndex();
    return Ok(std::move(map));
}

// ── ProGuard 兼容 map ──

Result<RenameMap, std::string> parseProguardMap(std::string_view text)
{
    RenameMap  map;
    std::string current;

    for (std::string_view raw : lines(text)) {
        const std::string_view line = trim(raw);
        if (line.empty()) continue;
        if (line.front() == '#') continue;

        const ca::usize arrow = line.find("->");
        if (arrow == std::string_view::npos) {
            ++map.skippedLines;
            continue;
        }
        std::string_view lhs = trim(line.substr(0, arrow));
        std::string_view rhs = trim(line.substr(arrow + 2));

        // 类行:`Old -> New:`(rhs 以 ':' 收尾)。
        if (!rhs.empty() && rhs.back() == ':') {
            rhs.remove_suffix(1);
            const std::string oldName = normalizeName(trim(lhs));
            const std::string newName = normalizeName(trim(rhs));
            if (oldName.empty() || newName.empty()) {
                ++map.skippedLines;
                continue;
            }
            current              = oldName;
            map.classes[oldName] = newName;
            if (oldName == newName) ++map.unchangedClasses;
            continue;
        }

        if (current.empty()) {
            ++map.skippedLines;
            continue;
        }

        MemberRename m;
        m.owner   = current;
        m.newName = std::string(trim(rhs));

        const ca::usize paren = lhs.find('(');
        if (paren != std::string_view::npos) {
            // 方法:`retType name(argTypes)`。名字是 '(' 之前最后一个 token。
            const std::string_view argsPart = lhs.substr(paren);
            std::string_view       head     = trim(lhs.substr(0, paren));
            const ca::usize        sp       = head.rfind(' ');
            if (sp == std::string_view::npos) {
                ++map.skippedLines;
                continue;
            }
            const std::string_view retType = trim(head.substr(0, sp));
            m.oldName                      = std::string(trim(head.substr(sp + 1)));
            m.isMethod                     = true;

            std::string desc = "(";
            if (argsPart.size() > 2) {
                std::string_view args = argsPart.substr(1, argsPart.size() - 2);
                for (const std::string_view arg : split(args, ',')) {
                    const std::string_view a = trim(arg);
                    if (a.empty()) continue;
                    desc += javaTypeToDesc(a);
                }
            }
            desc += ")";
            desc += javaTypeToDesc(retType);
            m.desc = std::move(desc);
        } else {
            // 字段:`type name`。名字是最后一个 token。
            const ca::usize sp = lhs.rfind(' ');
            if (sp == std::string_view::npos) {
                ++map.skippedLines;
                continue;
            }
            const std::string_view type = trim(lhs.substr(0, sp));
            m.oldName                   = std::string(trim(lhs.substr(sp + 1)));
            m.isMethod                  = false;
            m.desc                      = javaTypeToDesc(type);
        }

        if (m.oldName.empty() || m.newName.empty() || m.desc.empty()) {
            ++map.skippedLines;
            continue;
        }
        if (m.oldName == m.newName) ++map.unchangedMembers;
        map.members.push_back(std::move(m));
    }

    map.buildIndex();
    return Ok(std::move(map));
}

Result<RenameMap, std::string> parseRenameMap(std::string_view text)
{
    // 段头 `|Old|New|` 是 DashO 私有格式的强特征;否则按 ProGuard 的 `->` 行式。
    for (std::string_view raw : lines(text)) {
        const std::string_view line = trim(raw);
        if (line.empty()) continue;
        if (line.front() == '|') return parseDashoMap(text);
        break;
    }
    if (text.find("->") != std::string_view::npos) return parseProguardMap(text);
    return Err(std::string("无法判定 mapfile 格式(既无 `|Old|New|` 段头,也无 `->` 行)"));
}

}   // namespace dasho
