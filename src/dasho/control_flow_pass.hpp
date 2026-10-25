/// @file        control_flow_pass.hpp
/// @brief       dasho.ControlFlow：DashO 控制流注入（tryCatch 假异常）的通解剥离。
#pragma once

#include <string>

#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace dasho {

/// @brief 摘除 DashO `controlflow tryCatch` 注入的假异常表项与假异常类。
///
/// 不按"方法开头有个 goto 跳过一段 athrow"这类形态匹配——那是注入的**产物**，
/// 换个方法体量就变形。判据落在注入无法伪装的两件事上：
///   1. **假异常类**：classfile 45.3 + 0 字段 + 0 方法 + super=RuntimeException
///      （DashO 生成类的统一版本戳，与 SE 解密器同源）；
///   2. **异常表项**：`catch_type` 指向上述类。
///
/// 这类 handler 只捕获一个从未被实例化的异常类，语义上永不触发，摘除后行为
/// 不变。改写分两条路径，按"摘完之后方法还剩不剩真 handler"分工：
///   - 只剩空异常表 → **结构改写**：删除不可达的填充段与 handler 体，按新布局
///     再生 StackMapTable（无 handler 帧，不存在 verifier 保守帧失配问题）；
///   - 仍有真 handler → **保布局改写**：只摘异常表项，一条指令都不动，原帧
///     逐条仍成立（Strict 域重算的 handler 帧比 verifier 保守帧更精确，
///     重算反而会 VerifyError）。
class DashoControlFlowPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer& container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// 注册本 pass 的全部变换件（pack 装配期调用）。
ca::core::Result<void, std::string> registerControlFlow(mjbase::spi::PassNameRegistry& reg);

}   // namespace dasho
