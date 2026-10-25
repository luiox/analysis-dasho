/// @file        string_decrypt_pass.hpp
/// @brief       dasho.StringDecrypt：DashO 字符串加密的通解还原。
#pragma once

#include <string>

#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace dasho {

/// @brief 把 DashO 字符串加密的调用点折叠成明文 ldc。
///
/// 不依赖解密器名字/签名/所在类（DashO 每构建随机化：匿名 $1 类 + 无害通用名），
/// 判据是**调用点可静态求值**这一事实本身：
///   1. 锚点 = 返回 String 的 invokestatic（callee 纯且可解释）；
///   2. 区间 = 从锚点往前找最早的、以空栈起算仍恰好产出 1 个值的连续指令段
///      （覆盖外层密钥算术 + 不透明谓词 + 双支 ldc + 解密链）；
///   3. 求值 = 小型字节码解释器执行该区间；`new Random()` 用多个种子各跑一遍，
///      结果必须一致（不一致 = 结果依赖随机源，放弃）；
///   4. 改写 = 区间替换为一条 ldc 明文，死支诱饵随区间一并消失。
///
/// 任何一步不成立都跳过该站点，不做猜测性改写。
class DashoStringDecryptPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer& container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// 注册本 pass 的全部变换件（pack 装配期调用）。
ca::core::Result<void, std::string> registerStringDecrypt(mjbase::spi::PassNameRegistry& reg);

}   // namespace dasho
