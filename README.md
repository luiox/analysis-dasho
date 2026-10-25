# analysis-dasho

## What it is

DashO（PreEmptive Protection）混淆家族的 deobf 插件仓：mjbase pass-pack 静态链接 + 独立 CLI。输入混淆 jar，输出还原 jar。无宿主进程、无动态库边界，ABI 面不存在；pack 与 CLI 同源构建，跨仓混装产物由 mjbase 同源门拒绝。

## Pass 一览

| Pass | 作用 | 判据 |
|---|---|---|
| `dasho.StringDecrypt` | 把字符串加密调用点折叠成明文 `ldc` | 调用点可静态求值(纯方法 + 常量区间),**不依赖解密器名字/签名/所在类** |
| `dasho.ControlFlow` | 摘除 `controlflow tryCatch` 注入的异常表项与假异常类 | `catch_type` 指向**生成类**(classfile 45.3 + 0 成员 + `RuntimeException`),**不依赖注入形状** |
| `dasho.Checks` | 摘除运行时检查注入段(debug/emulator/hook/root/tamper/shelfLife) | **两层**:模板形状(反射三连 + `(seed*k)%seed` 谓词 + 响应)∧ 区间结构(hammock 单入口单出口) |
| `dasho.Cleanup` | 清除控制流填充:空转块、不可达死块、goto 接力、溢写对 | 窗口级**栈自足** + 逐指令**活跃性**(写下的槽在出口已死) |
| `dasho.Rename` | 按 mapfile 把混淆名还原成原名(类/成员/描述符/签名/反射常量/资源路径) | **不做判据**——改名不可逆推,只认构建时产出的 mapfile |

### `dasho.StringDecrypt`

不按 DashO 的调用点形态模板匹配,而是用一个小型 JVM 解释器直接求值调用点区间:外层密钥算术、不透明谓词、双支诱饵、伪内层解密全部被当作"待求值的常量表达式"一并处理。因此形态族谱的每一维(有无谓词、`ifeq`/`ifne` 极性、key 压栈位置、实参签名、解密器命名、链长、有无异常表)都不需要写代码。

两种改写策略按方法是否带异常表分工:

- **无异常表** → 整段折叠为一条 `ldc 明文`,死支诱饵一并消失,写回时按新布局再生 StackMapTable。
- **有异常表** → 原地改写:密文 `ldc` 换明文,解密调用换成等价的收栈序列(`pop` / `swap;pop`)。指令数与栈形不变,原帧(含 verifier 要求的保守 handler 帧)逐条仍然成立。

调用点折叠完之后还会清扫**死解密器类**(`Owner$1`):判据是"活指令里再无引用",且候选必须同时满足解密器形态(匿名类命名 + 无字段 + 无 `<init>` + 全 static + 至少一个返回 `String`)。不数常量池——libjbc 的池只追加,折叠后旧 `Methodref` 仍在,按条目计数一个也删不掉。

### `dasho.ControlFlow`

注入的 handler 捕获的是一个**从不被实例化的类**,所以摘掉表项是语义恒等的操作——这不是近似还原。判据只认两件注入无法伪装的事:`catch_type` 指向的类,以及那个类的生成指纹。

两种改写策略按"摘完之后异常表还空不空"分工(它等价于"帧还能不能重算"):

- **摘空** → 结构改写:从入口求可达性,删掉全部不可达指令(头部/尾部填充段 + handler 体),清掉"跳到紧邻下一条"的 `goto`,按新布局再生帧。
- **还剩真 handler** → 保布局改写:只摘表项,一条指令不动。真 handler 的保守帧不能重算(Strict 域比 verifier 精确,重算即 `VerifyError`)。

假异常类的删除另有三道门:写回全部成功、没有"保布局"方法的帧还引用它、没有指令 `new`/`checkcast` 它。

### `dasho.Checks`

检查注入没有专属标识——用的全是 JDK 类（`Class.forName` / `System.exit` 在正常代码里到处都是）。所以判据分两层，**同时成立才判**：

- **模板形状**（位置无关）：反射三连 `Class.forName` + `Class.getMethod` + `Method.invoke`、噪声谓词 `invokestatic ()I; dup; iconst; imul; swap; irem; if<c>`、响应 `System.exit` / `Runtime.halt` / `Thread.sleep` / `athrow`。
- **区间结构**：注入段被 `try { … } catch (Exception)` 整段包住，在 CFG 上必然是一个 **hammock**——区间外无入边、区间内后继要么在内要么恰好是出口、区间内每个位置要么从起点可达要么全局不可达（容忍 DashO 撒的死填充）。

起点从异常表推，但要**按 handler 分组**：`controlflow blockSplitting` 会把一条 `try` 拆成几十段表项而共用同一 handler，只取单段会把起点算到方法中段。右端还要额外要求**覆盖方法内全部 `forName` 锚点**，否则块切分产生的后半段会留在原地。

"区间外无入边"指的是**区间内部** `(f, X)`——外部边进入起点 `f` 必须允许。`where="End"` 时 DashO 把原表达式的值溢写到局部变量、注入段接在后面，必然有一条落下边进入 `f`；拒绝它会让所有 `End` 注入一条也剥不掉（实测踩过：6 处注入只剥掉 4 处）。唯一要拒的是回边（源在区间之后却跳回 `f`，说明是循环体）。

剥离顺序不可换：**先摘表项 → 再销毁只被摘除项引用的边界标签 → 最后删指令**。反过来会留下陈旧表项（校验器直接拒）或吃 `StillUsed`（异常表标签是 Boundary 角色，会保护自己的锚点）。摘完清死填充（否则帧再生不给死块发帧，产物 `VerifyError: Expecting a stack map frame`），再折叠 `where="End"` 留下的 `xSTORE n; xLOAD n` 溢写对。

### `dasho.Cleanup`

前三个 pass 把有语义的混淆都还原了，方法体里还剩一类**不改变程序状态、只破坏线性扫描**的代码：`controlflow blockSplitting`/`jumbling` 撒的 `ldc -1; istore N; ldc "0"; iinc N 1; astore N+1; goto L`。它对运行没影响，但反编译器会把它当成真实语句输出成 `int n = -1; ++n; String string = "0";`。

**不做块重排**——实测块序对反编译器是冗余的（CFR 自行 `aggressive block sorting`，Vineflower 连 CFR 都还原不了的 jumbled 嵌套循环也能完整还原），而重排会让异常表区间不再连续、必须拆分 try 范围。只做删除。

判据是"空转窗口"：**栈自足**（深度从 0 起全程非负、收在 `goto` 前回到 0——只按"净效应 0"判会误删 `pop; aconst_null` 这种把入口栈顶换成 null 的块）+ **写下的槽在出口已死**（逐指令活跃性，`liveIn = use ∪ (liveOut \ def)`）。用活跃性而不是"全方法不读"是因为 DashO 的填充常常复用真循环变量的槽。

改写用三步原子操作组合，不自己维护边表：`eraseBatch(窗口, kRetargetToFallthrough)` 把跳向窗口首的边改锚到窗口后的 `goto` → `threadGotos` 把指向它的边接到终点 → `pruneUnreachable` 收走孤立 `goto`。带异常表的方法**整体跳过**（帧再生不给死块发帧，留下即 `VerifyError`）。

### `dasho.Rename`

唯一一个**不做推断**的 pass:改名销毁了信息(`overload induction` 让多个原名共用一个新名,`randomize` 让每次构建名字都不同),精确还原只能靠 mapfile。因此它只在 pack 配置给了 `map` 时才注册,没有 mapfile 就什么都不做——不产出"看起来像原名"的合成名。

三件事最容易写错:反演方向(mapfile 是 `old→new`,计划要 `new→old`)、描述符预映像(源键用混淆空间的 `()Ll;`,目标键用原名空间的 `()LTuple;`,写错只是静默不命中)、改写面必须全开(漏一面就留悬垂引用,jar 能导出但运行期炸)。

设计与拒绝面见 `doc/string-decrypt-pass.md`、`doc/control-flow-strip-pass.md`、`doc/rename-pass.md`;技术形态见 `doc/string-encryption.md`、`doc/control-flow.md`、`doc/renaming.md`。

## Build

需要本地 mjbase 检出且含 plugin 接口（plugin-api 分支及以上），默认找 `../mjbase-plugin-api`，用 `MJBASE_DIR` 覆盖：

```sh
MJBASE_DIR=/path/to/mjbase-plugin-api xmake f --with_tests=y
xmake build dasho_cli
```

## Run

```sh
# 字符串加密 + 控制流注入 + 运行时检查 + 填充清理
xmake run dasho_cli -- -i in.jar -o out.jar \
    -p dasho.Checks,dasho.ControlFlow,dasho.StringDecrypt,dasho.Cleanup

# 再加改名还原（-m 等价于 -c 里写 {"map": "<路径>"}）
xmake run dasho_cli -- -i in.jar -o out.jar \
    -p dasho.Checks,dasho.ControlFlow,dasho.StringDecrypt,dasho.Cleanup,dasho.Rename -m build.map
```

`-p` 逗号分隔 pass 名（`dasho.` 前缀）；`-m`/`--map` 指定改名映射文件（DashO `.map` 或 ProGuard `.map`），也可用 `-c pack.json` 写 `{"map": "..."}`；`--list` 列出全部可用 pass；`--map-check <file>` 只解析 mapfile 并报统计。

**顺序有意义**：`dasho.ControlFlow` 必须在 `dasho.StringDecrypt` 之前——DashO 先做字符串加密再做控制流注入，注入打在了解密器自己身上，不先摘掉它就不"纯且可解释"（实测：顺序反了折叠 0 个站点）。`dasho.Checks` 与 `dasho.ControlFlow` **互不依赖、顺序无关**：两者锚点不同（`catch_type` vs `Class.forName`），也不互相误伤。`dasho.Cleanup` 放最后（`Rename` 之前）——它靠活跃性判断，前面几步把死块清掉之后判得更准。`dasho.Rename` 放最末。

排查"某个站点为什么没折"时开 `DASHO_TRACE=1`，站点级拒绝原因会打到 stderr（平时零开销）。

## Test

```sh
xmake build dasho_unittest && xmake run dasho_unittest
```

单测覆盖 `dasho.StringDecrypt` 的改写形状（形态 A / 形态 B 两种极性 / 带异常表 / 三种收栈形态 / custom SE / 嵌套 / 四类跳过）、`dasho.ControlFlow` 的剥离边界（结构改写 / 保布局 / 真 handler 共存 / 版本戳误判 / 指令引用保留）、`dasho.Checks` 的两层判据（整段剥离 + helper 清扫 / 缺噪声谓词 / 缺响应 / 区间有外部入边 / `where="End"` 落下入口 / helper 仍被活代码引用）、`dasho.Cleanup` 的空转窗口判据（整段删除 / 写的槽还活着 / 吃入口栈 / 不可达死块 / 带异常表跳过 / `goto` 跳回窗口首）、`dasho.Rename` 的反演与拒绝面（方向 / 描述符预映像含反证 / 同名跳过 / URI 冲突 / 描述符改写边界）、mapfile 双格式交叉校验、配置面解析，共 50 例。不覆盖解密算法——算法正确性由语料回归覆盖。

语料回归：

```sh
xmake build dasho_cli && ./test/corpus-regress.sh
```

对 `run-lab` 全部样本 jar 跑完整 pass 链（有同名 `.map` 时自动附 `dasho.Rename`），检查退出码、输出 jar 结构完整、类数不增，并在有 JDK 时对产物做**逐类加载校验**（`test/tools/VerifyJar.java`，每个类都过一遍校验器，含死代码——改写帧的 pass 出错往往落在没被执行到的分支上）。`CORPUS=<dir>` 指向别的语料目录，`NO_VERIFY=1` 跳过校验。

## Doc index

`doc/README.md` 是 DashO 技术档案索引。口径分工：`doc/` 只写**通用技术形态**；具体样本的取证过程（jar、类名、指令偏移、round-trip 还原值）记在仓外研究笔记，不进本仓。
