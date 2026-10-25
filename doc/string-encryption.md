# DashO 字符串加密(String Encryption)技术档案

本文描述 DashO 字符串加密这一**混淆技术的通用形态**:配置契约、调用点模板的族谱、解密器算法族、以及由形态直接推出的去混淆判据。

> **口径分工**(与仓外研究目录的边界):
> - 本文只写**技术形态**——"DashO 会产出什么样的字节码结构",与具体样本的类名/偏移/数值无关。
> - 具体样本的取证过程(jar 名、类名、指令偏移、round-trip 还原值、构建命令)记在仓外研究笔记;本文只保留可复算的**验收口径**(§7)。
> - 标注"实测"的结论来自对样例工程的构建与 javap 反汇编;标注"文档"的来自官方用户文档与 `dasho.xsd`。

## 1. 配置面(dasho.xsd `StringEncryptType`)

| 字段 | 类型/范围 | 默认 | 语义 |
|---|---|---|---|
| `level` | int 1–10 | **2** | 加密强度/运行时代价档位;10 最强最慢 |
| `implementations` | int 1–10 | **2** | **生成的加/解密实现数量**;名字与签名随机选取(除非输入 map);未合并输出时 × 输入 jar 数;`global` 属性可把解密器集中放全局 jar |
| `useIntern` | bool | false | 解密后调 `intern()` |
| `decrypter` | `DecrypterType` | — | 自定义解密器定位 |
| `includelist`/`excludelist` | `SupertypeClassListType` | — | 按超类/规则圈定范围 |
| `seInput.path`/`seOutput.path` | path | — | 解密器参数表读写(增量混淆用),格式见 §2.1 |
| `customEncryption` | `customEncryptionType` | — | 见 §5 |

## 2. 官方文档要点

- 机制:替换常量池字符串为密文,运行时解密。
- `implementations` 控解密器数量,且**名称/签名随机**——与 Allatori"一 jar 多解密器各自 key"同类现象,但 DashO 文档明示这是配置产物。
- **内联常量缺口(文档承认)**:javac 把 `static final` 字符串常量内联到使用点;DashO 不加密原字段值(怕破坏库 API 行为)⇒ 明文残留。要完全保护须配合 removal/排除;public/protected 字符串不会被移除。
  - 去混淆侧含义:输出里**必然存在未加密明文**,识别时不能假设"所有字符串都加密"。
- include/exclude 规则交叉时,跨规则复用的字符串可能不符合预期(文档 caveat);不写规则则全部方法加密。
- Android/R8 交互:SE 会让 R8 认不出反射类名字符串,需 rules/keep 或把含反射调用的方法排除出 SE。

### 2.1 `seInput` / `seOutput`:解密器参数表(实测)

官方文档只说这两个元素是 "String Encryption Input/Output Map File",不说内容。实测结论:**`seOutput` 产出的是「解密器参数表」,不是明文↔密文对照表**。`implementations=N` 就写 N 行,`++` 分隔,7 个字段:

```
<owner> ++ <decryptorClass> ++ <method> ++ <shape> ++ <keyStep> ++ <loopStep> ++ <keySlot>
```

| 字段 | 含义 |
|---|---|
| `owner` / `decryptorClass` / `method` | 宿主类 / 解密器类 / 解密方法名(三者都已混淆) |
| `shape` | 描述符形态:**1** = `(String,int)`、**2** = `(int,String)` |
| `keyStep` | 外层 key 步进,即 `toCharArray` 之前那条 `iinc <keySlot>, <keyStep>` |
| `loopStep` | 循环内 key 步进,即循环体里的 `iinc <keySlot>, <loopStep>` |
| `keySlot` | key 所在局部变量槽 |

`seInput` 的语义:把上一次的 map 喂回去重新构建,产出的解密器**逐字节相同**(类名、方法名、三个参数全一致)。所以这两个元素服务的是**增量混淆**——让未改动的字符串在新构建里保持同一套解密器参数。

**对还原工具的意义**:

- 不是查表捷径:map 里既没有明文,也没有"哪条密文用哪个解密器"的对应关系,调用点定位仍得靠 §6 的求值判据。
- 是**独立校验源**:可以拿 `keyStep`/`loopStep`/`keySlot` 反向核对解密器的 key 演化是否被正确理解,不必先逆向方法体。
- 若产物附带了 `.map`,可以直接按参数重建解密器——**mask 表达式不在表里**,它由同一 seed 确定性派生,仍需读方法体那 10 条指令。

## 3. 调用点形态族谱(实测)

DashO 的字符串调用点不是单一模板,而是**一族按档位分化的模板**。判别只需两件事:有没有不透明谓词,以及外层 int key 压栈的位置。

### 3.1 形态 A:直给(level=1 实测)

    ldc        <密文 String>
    bipush     <key:int>
    invokestatic <解密器>.<名>(String,int)String
    nop

无谓词、无分支。`nop` 是 DashO 的固定填充(占位/对齐),不是用户代码。

### 3.2 形态 B:不透明谓词 + 双支(level≥5 实测)

    <外层密钥算术>                      // 见 §3.3
    invokestatic <谓词源>.<名>()I       // r = Random.nextInt(75)+1,恒 ∈[1,75]
    dup
    iconst_N                            // N ≥ 2
    imul                                // r*N
    swap                                // 栈序 → [r*N, r]
    irem                                // (r*N) % r ≡ 0(r 作除数,恒 0)
    if<c> Ldead
    ldc        <真密文 String>          // 真支:单层 ldc
    goto       Ljoin
    Ldead:
    ldc        <诱饵密文 String>
    bipush     <常量 key>
    invokestatic <任一解密器>(...)String // 伪内层解密,死支专属
    Ljoin:
    invokestatic <外层解密器>(...)String
    nop

结构性事实:

- **代数恒定**:`(r*N) % r ≡ 0` 对任意 r≠0 成立,`r` 又恒 ≥1 ⇒ 该比较恒为真。判据只能**求值取支**,不能按助记符猜:极性 `ifeq`/`ifne` 每次构建随机。
- **死支存在的理由**:两支必须留下**同样深度**的栈进合并点(verifier 要求)。真支留一个 String;死支因此必须"造出"一个 String——于是有 `ldc 诱饵 + 常量 key + 伪内层解密`。
- **伪内层可用任一实现**,包括外层正在用的同一个方法。它不是真的第二层加密。
- 真支的密文**长度 = 明文长度**(逐字符 XOR 的直接后果),可作旁证但不可作判据。

### 3.3 外层 int key 的两种压栈位置(实测)

| 位置 | 形态 | 说明 |
|---|---|---|
| 谓词**前** | `bipush a; bipush b; iadd` 等,结果跨两支留在栈下 | key 在谓词求值前就压好,两支共用 |
| 合并点**后** | 谓词前无 key,`Ljoin` 之后才算 | 与真支 ldc 的相对顺序因此不固定 |

算术链形态**每站点随机**(SE 原生随机化,不带 optimization 也出现):简式 `iadd/imul`、`dup2/swap/iadd` 链、`dup_x1` 链都有。DashO 的 optimization **不折叠**这些链(实测)。

⇒ 折叠器必须**栈感知**(处理 `dup/dup2/dup_x1/swap`),且两个位置都要扫。

### 3.4 解密器的落点与命名(实测)

- 解密器是**用户类内部的匿名内部类** `X$1`,不是独立运行时库。`level ∈ {1,5,10} × implementations ∈ {1,2,3}` 全档位实测:**不注入 `lib/DecryptLib.jar`**。
- 输出类数 = 输入类数 + `implementations`(每实现一个 `$1` 类)。
- 方法名从无害 JDK 方法名里随机取:`replace`/`copyValueOf`/`subSequence`/`lastIndexOf`/`insert`/`regionMatches`/`concat`/`startsWith`/`endsWith`/`onsite` 等。
- 签名族:`(int,String)String`、`(String,int)String`、`(String,int,int)String`——实参个数与 int 的位置都随机。

⇒ **不能按名字、类名、签名匹配**。唯一稳定的判据是"调用点可静态求值"这一事实本身(§6)。

## 4. 解密器算法族(实测)

逐字符 XOR,参数每字符串不同:

    key += <k0>                        // iinc 在入参上改
    chars = ct.toCharArray()
    mask = <常量算术混淆序列>            // 形如 ((4+1)<<…)^32 → istore
    for i in 0..len-1:
        chars[i] ^= (key & mask)       // caload; iload key; iload mask; iand; ixor; i2c; castore
        key += <step>                  // iinc
    return String.valueOf(chars, 0, len)

算法级归纳:

    plain[i] = ct[i] ^ ((key_base + i*step) & mask)

`key` 逐步演化、`mask` 经 `ishl`/`ixor` 混淆;`k0`/`step`/`mask` 每字符串不同,不同实现的循环骨架**逐指令同构**。

对去混淆的含义:这条算法**不需要**被识别。见 §6。

## 5. Custom String Encryption(实测)

契约:

- 加密方法:`public static String[] enc(String plaintext)` → `{key, ciphertext}`(双非空);打包进独立 jar 配 `encryptionJar`。
- 解密方法:`public static String dec(String key, String ciphertext)` → 明文;**必须在项目 inputs 内**。
- 解密类本身仍可被改名/混淆,但排除于 Custom SE 之外;若它依赖输入内其它类,须手动排除以免递归。
- 无显式 include 规则时 Custom SE 不生效。

调用点形态(与内建形态截然不同):

    ldc        <key 明文 String>
    ldc        <密文 String>
    invokestatic <用户包>.<类>.<dec>(String,String)String
    nop

无谓词、无分支、无诱饵。`key`/`cipher` **明文双 ldc** 进常量池。

同一 jar 可**混布**两种形态:`option="on"` 时 custom 与内建按类分流——includelist 命中类走 custom,其余类走内建。此外,配置的 `decryptionMethod` 被整体排除出内建 SE(防运行期递归),因此用户解密器**自身方法体内的字符串是明文**——这是还原时定位用户解密器的天然锚点。

## 6. 去混淆判据(由形态推出,不依赖具体样本)

DashO 的形态族谱看起来分支很多(有无谓词、极性、key 位置、签名、名字、层数),但**逐条写判据会得到一个脆弱的分支树**。真正的通解只有一条:

> **调用点是纯的、可静态求值的常量表达式 ⇒ 求值它,用结果替换它。**

判据与处置:

1. **锚点** = 返回 String 的 `invokestatic`,且 callee 可判定为纯(无字段读写、无同步、无 `athrow`、调用面全部纯或 JDK 内建纯方法)。名字/签名/所在类一律不看。
2. **区间** = 从锚点往前找最早的、以空栈起算仍恰好产出 1 个值的连续指令段。这一段自动涵盖:外层 key 算术、谓词源调用、双支、密文 ldc、伪内层解密——**不需要单独识别其中任何一件**。
3. **求值** = 一个小型字节码解释器执行该区间。`new Random()` 用**多个种子各跑一遍**,结果必须一致——不一致说明该站点依赖随机源,放弃(这是"谓词恒真"的判定,而不是"识别 `(r*N)%r` 这个形状")。
4. **改写** = 区间替换为一条 `ldc 明文`。

这条判据天然覆盖:形态 A(无谓词)、形态 B(两种极性)、两种 key 位置、任意签名、任意名字、任意层数、以及 custom SE。它对形态族谱的每一维都不敏感——这正是"通解"的含义。

失败面(都会退化为"跳过该站点",不做猜测性改写):

- callee 不纯(读字段、写字段、同步、native/abstract/synchronized);
- 区间含不可判定的指令(字段访问、`invokedynamic`、`jsr/ret`);
- 多 seed 结果不一致;
- 求值结果不是合法 UTF-16 文本(拒 NUL、孤立代理项、C0/C1 控制符)——错误求值的产物几乎总带控制符。

设计与实现细节见 [string-decrypt-pass.md](string-decrypt-pass.md)。

### 6.1 反例与边界

- **静态死代码**形态(`ldc -1; istore; ldc "0"; iinc; astore; goto`)是 controlflow 侧同源不同形,见 [control-flow.md](control-flow.md),归通用死代码清除,不进字符串判据。
- **明文残留**:`static final` 内联常量与 `decryptionMethod` 体内的字符串不加密(§2、§5),扫描时应把它们当作"已经是明文"而不是"漏折的站点"。
- **不能假设所有 `invokestatic` 都能折**:JDK 方法本身(如 `String.valueOf`)也返回 String 且"纯",但它的实参不是常量,`evaluate` 会因栈净效应/求值失败而自然跳过——判据靠求值成功,不靠黑名单。

## 7. 验收口径(可复算,仓内自足)

本仓 `dasho.StringDecrypt` pass 的验收不依赖具体样本的取证细节,只依赖两条可复算的门:

1. **字节码合法性**:还原后的每个 jar,用 JDK 校验器加载全部类(`Class.forName(name, true, loader)`)必须 0 失败。这一条能挡住所有帧/栈深错误。
2. **语义等价**:对已知明文的样本,还原出的 `ldc` 常量必须等于原明文;对可 headless 启动的样本,原 jar 与还原 jar 的 stdout/stderr 必须一致。

具体样本清单、每个样本的还原结果与运行记录见仓外研究目录的 `findings/`;本仓只记录"哪些形态被覆盖"这一层结论:

| 形态 | 覆盖 |
|---|---|
| 形态 A(level=1,无谓词) | ✔ |
| 形态 B(`ifeq` 极性) | ✔ |
| 形态 B(`ifne` 极性) | ✔ |
| 带异常表的方法(原地改写路径) | ✔ |
| Custom SE `(String,String)` | ✔ |
| 嵌套调用(伪内层 + 外层) | ✔ |
| 未开 SE 的样本(应零改写) | ✔ |
