/// @file        cleanup_pass.hpp
/// @brief       dasho.Cleanup：控制流填充的规范化（死填充 / 空转块 / goto 接力）。
#pragma once

#include <string>

#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace dasho {

/// @brief 清掉 DashO 控制流变换留下的**无语义贡献**的代码。
///
/// 三个变换面都会往方法体里撒填充，形态不同但性质一样——不改变程序状态，
/// 只破坏反编译器的线性扫描：
///
/// | 来源 | 形态 | 本 pass 的处理 |
/// |---|---|---|
/// | `controlflow blockSplitting` / `jumbling` | `ldc -1; istore N; ldc "0"; iinc N 1; astore N+1; goto L` | 空转块旁路（判据见下） |
/// | `controlflow tryCatch` | `goto L; nop; nop; athrow; L:` 死块 | 不可达剪除 |
/// | 块切分本身 | 大量 `goto` 接力、`goto` 到紧邻下一条 | goto 线程化 + 相邻 goto 删除 |
///
/// **空转窗口判据**（三条同时成立，缺一即放弃）：
///
/// 1. **栈自足**：把窗口的入口栈深当作 0 逐条推演，深度全程非负且终值为 0。
///    只要求"净效应 0"不够——`pop; aconst_null` 净效应也是 0，但它把入口栈顶
///    换成了 null，旁路就会改语义。深度全程非负才排除了这种"吃入口栈"的窗口。
/// 2. **写下的槽在出口已死**：窗口写到的每个局部变量槽，在窗口出口处都不活跃
///    （逐指令活跃性分析，`liveIn = use ∪ (liveOut \ def)`）。不能用"这个槽全
///    方法都不被读"——DashO 的填充常常复用真循环变量的槽。
/// 3. **单出口**：窗口以 `goto L` 收尾，且 `L` 不在窗口内、不是窗口自身。
///
/// 另有一条结构性前提：窗口内部不得有分支目标（否则有边能跳进窗口中间，删了
/// 会悬垂）。**窗口不必是基本块**——DashO 会把填充插在实参求值中间
/// （`aload_0; getstatic x; ldc -1; istore_1; …`），所以判据做到指令窗口级。
///
/// 只处理**没有异常表**的方法。有真 handler 时，删掉一条指令就可能让 handler
/// 体变成"只被异常边可达"，而帧再生只给可达目标发帧——留下死块即
/// `VerifyError: Expecting a stack map frame`。这类方法整体跳过，宁可不改。
class DashoCleanupPass final : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer& container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

/// 注册本 pass 的全部变换件（pack 装配期调用）。
ca::core::Result<void, std::string> registerCleanup(mjbase::spi::PassNameRegistry& reg);

}   // namespace dasho
