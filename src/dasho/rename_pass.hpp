/// @file        rename_pass.hpp
/// @brief       dasho.Rename：按 mapfile 把混淆名还原成原名。
///
/// 与另外两个 pass 的分工：字符串加密、控制流注入都是**语义级**还原（不认名字），
/// 本 pass 恰恰只认名字——因为 DashO 的改名是**有损**的（`doc/renaming.md`
/// §生成名的分配规则），单字母类名不含任何可推导信息，精确还原的唯一来源是
/// 构建时产出的 mapfile。
///
/// 因此本 pass 的输入不是"判据"，而是一份**外部证据**。没有 mapfile 时它拒绝
/// 做任何猜测性改写：宁可不动，也不产出"看起来像原名"的假名字。
#pragma once

#include <memory>
#include <string>

#include "dasho/rename_map.hpp"

#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace dasho {

/// @brief 按 mapfile 反演出的计划改写容器内的类名/成员名/资源路径。
///
/// 改写面（全部开启，缺一不可）：
///   - 字节码：this/super/interfaces、成员声明与引用、指令操作数、异常表
///     catch_type、StackMapTable 里的 Object 校验类型、内部类/Nest 族元数据；
///   - 反射字符串：`Class.forName("a")`、`getMethod("x")` 这类常量；
///   - 注解与泛型签名：注解元素值、Signature 属性、LVTT 行；
///   - 资源：`a.class` → `原名.class`（含包路径重建）、`META-INF/services`
///     的 provider 行、MANIFEST 的 `Main-Class` 族。
///
/// 为什么这些面必须一起开：类改名后只要漏一面就留下悬垂引用，jar 能导出但
/// 运行期炸在 ClassNotFound/NoSuchMethod 上——**半改比不改更糟**。
class DashoRenamePass final : public mjbase::spi::GlobalPass
{
public:
    explicit DashoRenamePass(std::shared_ptr<const RenameMap> map) : map_(std::move(map)) {}

    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer& container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;

private:
    std::shared_ptr<const RenameMap> map_;
};

/// 注册本 pass（pack 装配期调用）。map 由 pack 从配置指向的 mapfile 解析得到。
ca::core::Result<void, std::string> registerRename(mjbase::spi::PassNameRegistry& reg,
                                                   std::shared_ptr<const RenameMap> map);

}  // namespace dasho
