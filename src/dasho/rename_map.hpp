/// @file        rename_map.hpp
/// @brief       改名映射文件(mapfile)解析:两种格式 → 统一改名计划。
///
/// DashO 的 renaming 有损:单字母类名不含任何语义,精确还原**只能靠 mapfile**。
/// 本单元是还原链的第一环——把两种格式读成同一张计划表,后续变换件只认计划表,
/// 不认格式。
///
/// 两种格式由同一次构建同时产出(见 doc/renaming.md §mapfile):
///   - DashO 私有 `.map`:`|Old|New|` 段头 + `=old=desc=new=`(方法)/
///     `+old+desc+new+`(字段),desc 是 JVM 描述符;
///   - ProGuard 兼容 `.proguard.map`:`old -> new:` + `retType name(args) -> new`。
///
/// 差异全部收在这里:DashO 格式自带描述符,ProGuard 格式要把 Java 风格签名
/// 转成 JVM 描述符。转出来的结果与 DashO 格式逐字节一致,交叉校验见单测。
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

namespace dasho {

/// 一条成员改名记录。
struct MemberRename
{
    std::string owner;     ///< 旧类内部名(`'/'` 分隔)
    std::string oldName;
    std::string desc;      ///< JVM 描述符
    std::string newName;
    bool        isMethod = true;
};

/// 改名计划:类表 + 成员表 + 成员索引。
class RenameMap
{
public:
    /// 旧类内部名 → 新类内部名(恒等映射也收进来,便于统计"没改名的")。
    std::unordered_map<std::string, std::string> classes;
    std::vector<MemberRename>                    members;

    /// 新旧同名的类/成员计数(入口点、`<init>`、接口方法、`serialVersionUID` 等)。
    ca::usize unchangedClasses = 0;
    ca::usize unchangedMembers = 0;

    /// 语法不合而跳过的行数(解析是容错的:一行坏不影响其余)。
    ca::usize skippedLines = 0;

    bool empty() const noexcept { return classes.empty() && members.empty(); }

    /// 成员新名;未命中返回空视图(未改名 / 不在 map 里)。
    std::string_view memberNewName(std::string_view owner, std::string_view name,
                                   std::string_view desc) const;

    /// 类新名;未命中返回空视图。
    std::string_view classNewName(std::string_view oldName) const;

    /// 按成员表建索引(各解析入口收尾时调用;重复调用幂等)。
    void buildIndex();

private:
    std::unordered_map<std::string, ca::usize> memberIndex_;
};

/// 解析 DashO 私有 `.map`。语法不合的行跳过并计入 `skippedLines`,不整体失败。
ca::core::Result<RenameMap, std::string> parseDashoMap(std::string_view text);

/// 解析 ProGuard 兼容 map(Java 风格签名 → JVM 描述符)。
ca::core::Result<RenameMap, std::string> parseProguardMap(std::string_view text);

/// 按内容判格式:DashO 段头(`|Old|New|`)优先,否则按 ProGuard(`old -> new:`)。
ca::core::Result<RenameMap, std::string> parseRenameMap(std::string_view text);

/// Java 风格类型 → JVM 描述符(`java.lang.String[]` → `[Ljava/lang/String;`)。
/// 未识别的形态按对象类型处理(`com.foo.Bar` → `Lcom/foo/Bar;`)。
std::string javaTypeToDesc(std::string_view type);

}   // namespace dasho
