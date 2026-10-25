# DashO 与 Allatori 技术对照

本文给出 DashO 12.8 与 Allatori 混淆家族在去混淆相关维度上的技术对照,作为处理两类混淆产物的先验参考。Allatori 侧结论来自对其发行版的实测,DashO 侧结论见本目录各分册。

| 维度 | Allatori(实测) | DashO 12.8(实测) |
|---|---|---|
| 解密器数量 | 多解密器各自 key(实测) | implementations 1–10 可配,默认 2 |
| 强度档 | fast/strong 两形态(4.x) | level 1–10 连续档 |
| 明文残留 | 无加密则无 | **内联常量明文残留**(设计如此) |
| synthetic | 判据已不依赖 | make-synthetic 会主动滥用 |
| 重命名 | 关键字名(9.9 demo)/i/I(8.x) | 自定义字母表 + randomize |
| 运行时库 | 解密逻辑在类内 | 字符串加密解密器在用户类匿名 `$N`;**任何 level×implementations 都不注入 DecryptLib**,仅 Android resourceencryption 触发(见 [string-encryption.md](string-encryption.md)) |
| opaque 谓词 | 输出常见 opaque 守卫残迹(静态死代码,可归通用清理) | level≥5 调用点**必带 ≡0 opaque 谓词** + 死支伪内层 decoy(按求值取真支;level=1 无谓词直给,见 [string-encryption.md](string-encryption.md)) |
| 解密算法 | 逐字符 XOR + key 演化 | 逐字符 XOR + key 演化 + ishl/ixor 掩码混淆(骨架同构,参数不同) |

## 去混淆侧通用结论

- 常量折叠前置、调用链解析、逐字符 XOR 解密骨架对两者直接适用;DashO 新增:≡0 opaque 谓词模板识别(按求值取真支:ifne→fallthrough/ifeq→target)、死支伪内层诱饵链(真支恒单层)。
- 输出残留的分类方法(常量垃圾/类型残留/opaque 死代码)对两者同样适用。
- **opaque 守卫的清偿归属不同**:Allatori 侧的 opaque/死分支残迹属静态死代码,归通用优化/死代码清理即可;DashO 调用点的 ≡0 opaque + 死支伪内层 decoy 是字符串还原的**前置必解**(按求值取真支才能拿到密文,见 [string-encryption.md](string-encryption.md) 调用点候选判据)——同类目、不同清偿归属;DashO 侧 controlflow 静态死代码(见 [control-flow.md](control-flow.md))仍归通用死代码清除。
