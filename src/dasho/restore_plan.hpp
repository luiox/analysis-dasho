/// @file        restore_plan.hpp
/// @brief       mapfile → 反向重映射计划：把 old→new 的改名记录反演成 new→old。
///
/// 这一层是改名还原的**全部风险所在**，因此做成不碰容器、不碰 IO 的纯函数，
/// 单测直接喂文本断言计划内容。
///
/// 三个必须做对的地方：
///
/// 1. **反演方向**。mapfile 记的是 原名 → 混淆名。还原要按 混淆名 → 原名 改写，
///    所以每条规则都要把首尾两段对调，而不是把 mapfile 直接灌进计划。
///
/// 2. **描述符预映像**。mapfile 里的描述符是**原名空间**的（`()LTuple;`），而
///    混淆后的类文件里同一方法是 `()LJi;`。重映射计划按 (owner,name,desc) 精确
///    匹配，源键必须用混淆空间的描述符，目标键才用原名空间的——两边都得给对，
///    给错任何一边都表现为"规则静默不命中"，不报错、不改、白跑一趟。
///
/// 3. **同名条目**。`<init>`/`<clinit>`/接口实现方法/`serialVersionUID`/入口类
///    在 mapfile 里是 `old == new` 的全等行；反演后仍是恒等，进计划只会让
///    remapper 多扫一遍，直接跳过。
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/asm/remapper/member_remap_plan.hpp"

#include "dasho/rename_map.hpp"

namespace dasho {

/// 一次反演装载的计数（诊断面；pass 汇总后打日志）。
struct RestorePlanStats
{
    ca::usize classRules      = 0;  ///< 落计划的类改名条数（不含同名）
    ca::usize methodRules     = 0;
    ca::usize fieldRules      = 0;
    ca::usize identitySkipped = 0;  ///< 新旧同名，无需改
    ca::usize unmappedOwner   = 0;  ///< 成员所属类不在类表里，无法定位到混淆名
    ca::usize badDescriptor   = 0;  ///< 描述符语法坏，跳过该条
    ca::usize uriConflict     = 0;  ///< 目标 URI 已被占，整类放弃
};

/// 反演装载的输入开关。
struct RestorePlanOptions
{
    /// 目标 URI 已被占用的类（**混淆名**）。这些类不改名，其成员规则也不装：
    /// 类名不动则成员规则的 owner 仍是混淆名，反演目标会和现有条目撞车。
    /// 由 @ref findClassUriConflicts 在容器上算出。
    std::unordered_set<std::string> skipObfClasses;
};

/// 按 `mapName` 改写描述符里全部 `L...;` 段的类名（数组前缀与其余字节原样）。
///
/// 描述符语法非法（`jbc::type::classNameSpans` 拒绝）时返回 Err——**不静默返回
/// 原串**：调用方拿不到"改没改"的信息就会把规则装错空间。
ca::core::Result<std::string, std::string>
rewriteDescriptor(std::string_view desc,
                  const std::function<std::string_view(std::string_view)>& mapName);

/// 把 mapfile 反演成"混淆名 → 原名"的重映射计划。
///
/// 先装全部类映射再装成员规则：成员规则的 owner/desc 要按类映射预映像，
/// 顺序反了会得到"半映射"的键（见 mjbase `rename_plan.hpp` 的预映像注记）。
RestorePlanStats buildRestorePlan(const RenameMap&                       map,
                                  mjbase::asm_remapper::MemberRemapPlan& plan,
                                  const RestorePlanOptions&              options = {});

/// 找出"目标 URI 已被现有资源占用"的类（返回**混淆名**集合）。
///
/// 触发场景是 mapfile 与 jar 不配套：例如把 `a → com/foo/Bar` 应用到同时含
/// `a.class` 与 `com/foo/Bar.class` 的容器，两个类会抢同一个 URI，后写的覆盖
/// 先写的——静默丢类。这里按 URI 层面提前判定，不解析任何类字节。
///
/// @param uris 容器内全部资源 URI（`ResourceContainer::visitResources` 收集）。
std::unordered_set<std::string>
findClassUriConflicts(const RenameMap& map, const std::vector<std::string>& uris);

}   // namespace dasho
