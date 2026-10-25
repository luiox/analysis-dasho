# DashO 12.8 技术档案:总览与索引

本档案描述 DashO 12.8.0.12560(PreEmptive Solutions)Java 混淆器的**技术形态**,面向字节码去混淆与对抗分析。按功能面拆分为多文件,本文件为入口与公共背景。

## 口径分工(重要)

本目录只写**通用的混淆技术描述**——"DashO 会产出什么样的结构",与具体样本的类名、指令偏移、还原数值无关。写作判据:**任何一条结论,换个输入 jar 仍然成立,才写在这里**。

具体样本的**取证过程**(哪个 jar、哪个类、哪条指令、round-trip 还原出什么、构建命令)以及所有**测量数值**(字节数、类数、分档矩阵、类名映射表)记在仓外研究笔记,不进本仓。本仓只保留可复算的**验收口径**(见 [string-encryption.md](string-encryption.md) §7)。

- 标注「实测」= 对样例工程构建后反汇编得到的**形态**结论(形态可复算,取证细节在仓外)。
- 标注「文档」= 官方用户文档与 `dasho.xsd` 配置 schema。
- 无标注 = 由前两者推出的结构性结论。

例外:[analysis-toolchain.md](analysis-toolchain.md) 写的是**本仓自己的研究工具链**(原型层),既不是 DashO 技术形态也不是样本取证,但它是复现上述所有判据的前提,所以留在仓内。

## 文档索引

| 文件 | 内容 |
|---|---|
| [README.md](README.md) | 本文件:发行版结构、CLI、配置面总览 |
| [string-encryption.md](string-encryption.md) | 字符串加密:配置契约、调用点形态族谱(直给/谓词双支)、外层 key 位置、解密器算法族、Custom SE、去混淆判据、验收口径 |
| [string-decrypt-pass.md](string-decrypt-pass.md) | **本仓 `dasho.StringDecrypt` pass 的实现设计**:求值式判据、两条改写策略、不变量来源、失败面 |
| [control-flow.md](control-flow.md) | 控制流混淆:块乱序/切块/try-catch 注入、假异常类指纹、剥离判据 |
| [control-flow-strip-pass.md](control-flow-strip-pass.md) | **本仓 `dasho.ControlFlow` pass 的实现设计**:catch_type 判据、两条改写策略、可达性 fail-closed、删类三道门 |
| [renaming.md](renaming.md) | 重命名:字母表/随机化/包扁平/mapfile、生成名的分配规则(双射 N 进制 / 类名按原名升序 / overload induction / 大小写折叠) |
| [rename-pass.md](rename-pass.md) | **本仓 `dasho.Rename` pass 的实现设计**:反演方向、描述符预映像、改写面全开、三道拒绝门、mapfile 里的编造名、已知残留差异 |
| [resource-encryption.md](resource-encryption.md) | 资源加密:AES/assets/raw(Android) |
| [checks.md](checks.md) | 运行时检查框架:debug/emulator/hook/root/shelf/tamper、注入模板、hammock 判据 |
| [checks-strip-pass.md](checks-strip-pass.md) | **本仓 `dasho.Checks` pass 的实现设计**:两层判据(模板 + hammock)、按 handler 分组定起点、剥离三步顺序、死填充与死类清扫 |
| [cleanup-pass.md](cleanup-pass.md) | **本仓 `dasho.Cleanup` pass 的实现设计**:空转窗口判据(栈自足 + dead-store)、活跃性分析、goto 线程化、为什么不做块重排 |
| [removal-optimization.md](removal-optimization.md) | 裁剪/优化/make-synthetic/方法调用移除 |
| [watermark-premark.md](watermark-premark.md) | 水印与 PreMark |
| [decryptlib.md](decryptlib.md) | DecryptLib 运行时注入库逐类勘察 + 算法重写 |
| [allatori-comparison.md](allatori-comparison.md) | 与 Allatori 的技术对照 |
| [analysis-toolchain.md](analysis-toolchain.md) | 分析工具链(`mjt-mcp`):原型层为什么存在、API 面、四个坑 |

## 发行版结构与组件

| 组件 | 大小 | 说明 |
|---|---|---|
| DashOPro.jar | 8.47MB | 主程序,`Main-Class=DashOPro`,Ant/JDK8(1.8.0_202)打包;manifest Class-Path 依赖 lib/asm-9.9 全家桶 + groovy-3.0.13 + jaxb + wizard;`Add-Opens: java.base/java.lang`(兼容新 JDK) |
| DashOProGUI.jar | 1.24MB | GUI 壳 |
| ant-dasho.jar | 43KB | Ant task(antconfig/antinstall 配套脚本) |
| Encrypter.jar | 1KB | 仅 `ResourceEncrypter.class`(499B),资源加密密钥生成侧 |
| lib/DecryptLib.jar | 20KB | **运行时注入解密库**:`com/preemptive/dasho/decryptlib/Wrapper` + `dasho/a..m`(混淆名 13 类,含 `$1/$a/$b` 匿名/嵌套)——详见 [decryptlib.md](decryptlib.md) |
| lib/shelflife-java.jar / shelflife-android.jar | 31KB | 有效期检查(shelf life check)实现 |
| lib/bootstrap-runtime.jar | 1KB | 运行时引导 |
| lib/dasho-annotations.jar | 10KB | 用户注解(`@Entry` 等入口标注) |
| ConvertMap.jar / mapfile.jar | 20KB/22KB | 重命名映射文件转换工具 |
| Lucidator.jar | 389KB | 报告/stack trace 解码配套工具(官方文档有专章) |
| tokengenerator / premark | 脚本 | shelf life token 生成器 / 水印工具 |
| gradle/*.jar | 51 个版本 | Android/Java Gradle 插件演进(1.4.1 → 6.0.6) |
| tools/ | — | dex-tools-v2.4、apktool 2.7.0、bundletool 1.18.2(Android 支线) |
| docs/ | 129 html | 完整用户文档(含 dasho.xsd 配置 schema) |

## 运行与 CLI

- 入口:`dashocmd [options] [projectfile]` → `java -Djava.awt.headless=true -jar DashOPro.jar <args>`(dashocmd.bat);选项必须 `-` 开头;`--quiet` 可静默。
- `dashorc`(安装目录/`$HOME`/`%USERPROFILE%` 逐级加载):持久化 `javaOptions/memoryStart/memoryMax/stackSize/options`、代理配置。
- 其他 CLI:`dashogui`(GUI+mapfile 解码)、`ConvertMap.jar --help`、`tokengenerator`、`premark inputfile`;Ant(`ant-dasho`)与 Gradle 集成。
- Windows 下 `--version/--help` 输出不进控制台(已知怪癖,官方文档明示)。
- `--file-version` 输出项目文件格式版本(当前 11.3.0,≠ 产品版本)。
- 跑通要点:项目文件根元素 version 必须填文件格式版本(11.3.0,`--file-version` 输出值),且必须配 `<classpath useJDKHome="true" JDKHome="..."/>`;详见 [string-encryption.md](string-encryption.md)。

## 配置面(docs/dasho.xsd,92KB)

顶层顺序(节选):`inputpath/classpath/classes/entrypoints/removal/controlflow/stringencrypt/resourceencryption/make-synthetic/optimization/premark/checks/output/signing/watermark/...`。根元素 `<dasho version="...">`。

各功能域复杂类型名称级全景(细节见各分册):

- **stringencrypt** → [string-encryption.md](string-encryption.md):`level`(1–10,默认 2)/`implementations`(1–10,默认 2)/`useIntern`/`decrypter`/`includelist`/`excludelist`/`seInput`/`seOutput`/`customEncryption`
- **controlflow** → [control-flow.md](control-flow.md):`blockSplitting`/`blockSplittingBlockSize`/`jumbling`/`tryCatch`/`catchHandlers`
- **renaming** → [renaming.md](renaming.md):`rename`/`alphabet`/`minlength`/`randomize`/`keeppackages`/`keeppublics`/`mapping(mapinput/mapoutput)`/`proguardMap`/`entryPointRename`
- **resourceencryption** → [resource-encryption.md](resource-encryption.md):`assetencrypt`/`rawresourceencrypt`
- **checks** → [checks.md](checks.md):`debuggingCheck`/`debugEnabledCheck`/`emulatorCheck`/`hookCheck`/`rootCheck`/`shelfLifeCheck`/`tamperCheck` + 各 `*Response`(可带 `probability`)
- **removal / optimization / make-synthetic / methodCallRemoval** → [removal-optimization.md](removal-optimization.md)
- **premark / watermark** → [watermark-premark.md](watermark-premark.md):`sigfile`/`charset`/`passphrase`
- expiry:`expirationDate`/`expirationPeriod`/`warningDate`
- output/signing:`keystore`/`keypass`/`storepass`/`signjar`
