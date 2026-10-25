# DashO 水印与 PreMark 技术档案

> **状态:暂缓(2026-10-23)**。水印是**有意写入**的追踪信息,不属于混淆语义,还原它
> 既非必要也会破坏溯源;`premark` 对 Android 产物的处理同样依赖 mjbase 尚缺的 Android
> 设施。本仓不实现水印的读取/摘除。本文保留文档级结论,见 `REPORT.md §1.1 覆盖矩阵`。

本文是 DashO 12.8 水印(watermark)与 PreMark 的技术说明,结论为文档级。

## 配置

- premark(dasho.xsd PremarkType):`sigfile`/`charset`/`passphrase` 等。
- watermarking 两路:GUI 内嵌(prebuild,每个输出 jar 同一水印)或 `premark` CLI 后置(可对任意 jar 加/读水印,不需 DashO 混淆过)。

## 官方文档要点

- 不可察觉地嵌入数据,不影响运行行为;典型用途:序列号追踪分发副本。
- 5 种预定义字符表(PreMark),复杂字符集每位宽更大 → 可嵌字符更少。
- 可选 passphrase 加密水印(占空间更多)。
- jar 容量不足时可配置:失败或截断尽量嵌入+告警。
- `tokengenerator` 配合 shelf life token;`premark` 独立 CLI。

## 去混淆侧影响

- 水印不改变代码语义,对字符串解密 pass 无直接影响;
- 但 watermark 字符串可能作为"额外常量"出现,识别时勿混淆为业务字符串。
