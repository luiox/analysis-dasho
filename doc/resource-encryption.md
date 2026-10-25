# DashO 资源加密(Resource Encryption)技术档案

> **状态:暂缓(Android 支线,2026-10-23)**。本特性只在 Android mode 存在,mjbase 目前
> 只有 jar/class 资源设施,没有 dex/apk/Android 资源模型,因此本仓不实现还原。本文
> 保留文档级结论,待 mjbase 具备 Android 设施后再启动。暂缓范围与影响面见
> `REPORT.md §1.1 覆盖矩阵`。

本文是 DashO 12.8 资源加密(resourceencryption)的技术说明。该特性仅 Android mode 可用(Java SE 无此特性),当前结论为文档级,运行时注入形态的逐类勘察见 [decryptlib.md](decryptlib.md)。

## 配置面(dasho.xsd ResourceEncryptionType)

- `assetencrypt`(bool):加密 Android assets。
- `rawresourceencrypt`(bool):加密 Android raw resources。
- 仅 Android mode(Java SE 无此特性);Standard Mode(标准 Java JAR)下该元素被忽略(官方文档注记)。

## 官方文档要点

- AES 加密 assets/raw;构建期注入解码流包装代码(包装 AssetManager/Resources.open* 流);密钥类同样混淆。
- 密钥可经环境变量传入(Gradle)避免日志泄漏。
- AAR 库二次处理有坑(文档专述)。
- 发行版内 `Encrypter.jar`(仅 ResourceEncrypter.class,499B)是加密侧密钥生成工具;运行时解密侧代码注入到输出 APK,形态为 lib/DecryptLib.jar 注入(逐类勘察、流头格式与算法重写见 [decryptlib.md](decryptlib.md))。
