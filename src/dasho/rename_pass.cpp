#include "dasho/rename_pass.hpp"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "dasho/restore_plan.hpp"

namespace dasho {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

std::vector<std::string> collectUris(const mjbase::model::ResourceContainer& container)
{
    std::vector<std::string> uris;
    container.visitResources([&uris](std::string_view uri, const mjbase::model::IResource&) {
        uris.emplace_back(uri);
    });
    return uris;
}

}  // namespace

mjbase::spi::PreservedAnalyses
DashoRenamePass::run(mjbase::model::ResourceContainer& container,
                     mjbase::spi::GlobalAnalysisManager& am)
{
    static_cast<void>(am);

    if (!map_ || map_->empty()) {
        CA_LOG_WARN("[dasho.Rename] 未提供 mapfile：改名不可逆推，本 pass 不做任何改写");
        return mjbase::spi::PreservedAnalyses::all();
    }

    // ── 1. 目标 URI 冲突预检 ──
    // mapfile 与 jar 不配套时（拿错构建的 map、jar 已被别的工具改过），两条
    // 类规则可能抢同一个 `原名.class`。容器是 map，后写覆盖先写——不预检就是
    // 静默丢类。冲突类整类放弃（含其成员规则），其余照常还原。
    const std::unordered_set<std::string> conflicts =
        findClassUriConflicts(*map_, collectUris(container));

    // ── 2. 反演成重映射计划 ──
    RestorePlanOptions options;
    options.skipObfClasses = conflicts;

    mjbase::asm_remapper::MemberRemapPlan plan;
    const RestorePlanStats              stats = buildRestorePlan(*map_, plan, options);

    if (plan.empty()) {
        CA_LOG_WARN("[dasho.Rename] mapfile 未命中任何可还原项（类 {} / 成员 {} 条规则）："
                    "多半是 map 与 jar 不配套",
                    map_->classes.size(), map_->members.size());
        return mjbase::spi::PreservedAnalyses::all();
    }

    // ── 3. 应用 ──
    // 全开改写面：漏一面就留悬垂引用（见头文件"为什么这些面必须一起开"）。
    const auto result = mjbase::asm_remapper::applyResourceRemapPlan(
        container, plan, mjbase::asm_remapper::RemapPolicy::all());
    if (result.is_err()) {
        CA_LOG_ERROR("[dasho.Rename] 改写失败（{}）：容器保持未完成状态，请勿采用本输出",
                     mjbase::asm_remapper::to_string(result.unwrap_err()));
        return mjbase::spi::PreservedAnalyses::none();
    }

    CA_LOG_INFO("[dasho.Rename] 类 {} 条、方法 {} 条、字段 {} 条规则；跳过同名 {} 条、"
                "无类表 {} 条、坏描述符 {} 条、URI 冲突 {} 条",
                stats.classRules, stats.methodRules, stats.fieldRules, stats.identitySkipped,
                stats.unmappedOwner, stats.badDescriptor, stats.uriConflict);

    if (!result.unwrap()) {
        // 计划非空但容器零变化 = 规则一条都没命中。最可能的原因是 mapfile 与
        // jar 不配套（拿错构建的 map）。这种"静默不命中"必须显式告警，否则
        // 用户看到上面的"N 条规则"会以为已经还原了。
        CA_LOG_WARN("[dasho.Rename] 规则已装载但容器零变化：mapfile 与 jar 可能不配套");
        return mjbase::spi::PreservedAnalyses::all();
    }
    return mjbase::spi::PreservedAnalyses::none();
}

Result<void, std::string> registerRename(mjbase::spi::PassNameRegistry&  reg,
                                         std::shared_ptr<const RenameMap> map)
{
    if (!map || map->empty()) {
        return Err(std::string("dasho.Rename 需要 mapfile（pack 配置项 \"map\"）"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = reg.registerGlobalPass(
            "dasho.Rename",
            [map] { return std::make_unique<DashoRenamePass>(map); },
            "Restore DashO renaming from its mapfile (DashO .map or ProGuard .map): reverse the"
            " old->new table into new->old and rewrite classes, members, descriptors, signatures,"
            " reflection strings and resource paths. Needs a mapfile; never guesses names.");
        r.is_err()) {
        return Err(std::string("dasho.Rename"));
    }
    return Ok();
}

}  // namespace dasho
