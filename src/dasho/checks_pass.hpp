/// @file        checks_pass.hpp
/// @brief       dasho.Checks：DashO 注入式运行时检查（checks）的通解剥离。
#pragma once

#include <string>

#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace dasho {

/// @brief 摘除 DashO `injection` + `*Check` 注入到方法里的检查段。
///
/// 不按"方法开头有一段反射三连"这类**单点指纹**匹配——真实业务代码里
/// `Class.forName` + `getMethod` + `invoke` 也常见，单点判据会误删活代码。
/// 本 pass 的判据是**结构 + 模板**两层同时成立：
///
/// 1. **结构**：注入段是一个 hammock（单入口、单出口的连续区间）。区间内每条
///    指令的后继都落在区间内或恰好是区间右端；区间外没有任何边进入区间内部；
///    区间内每个位置都能从入口到达（不许顺带吞掉无关死块）。
/// 2. **模板**：区间内同时含有
///    - 反射三连：`Class.forName` / `Class.getMethod` / `Method.invoke`；
///    - 噪声谓词：`invokestatic ()I` + `dup` + 整型常量 + `imul` + `swap` +
///      `irem` + 条件跳转（DashO 用"种子 × k % 种子 ≡ 0"造恒真分支，这个
///      七条窗口是注入模板本身，不是产物形态，改方法体量不会变形）；
///    - 响应：`System.exit` / `Runtime.halt` / `Thread.sleep` / `athrow`。
///
/// 三条模板判据缺一即放弃：宁可漏摘，不可误删。检测到之后整段删除，并连同
/// 整条落在区间内的异常表项一起摘掉（部分重叠一律放弃——改边界等于改保护
/// 语义）。
///
/// 删除完成后做死类清扫：注入 helper（`Owner$<数字>`，形态为"解密器 + 种子
/// 生成器"两个静态方法）与检查运行时门面（全 static native + 无实例字段）
/// 在活指令里再无引用时一并删掉。
class DashoChecksPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer& container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// 注册本 pass 的全部变换件（pack 装配期调用）。
ca::core::Result<void, std::string> registerChecks(mjbase::spi::PassNameRegistry& reg);

}   // namespace dasho
