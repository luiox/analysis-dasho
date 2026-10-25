# `dasho.Checks`:通解检查注入剥离 pass 的设计

本文是 `dasho.Checks` 的实现说明:**为什么判据必须落在"区间结构 + 模板形状"两层而不是单点指纹**、入口为什么只能从异常表推、以及剥离时"表项 → 标签 → 指令"的删除顺序为什么不能换。技术形态本身见 [checks.md](checks.md)。

## 1. 设计立场:认注入模板,但模板必须落在一个可判定的区间里

`checks` 注入和 `controlflow tryCatch` 的产物有本质区别:

| | controlflow tryCatch | checks |
|---|---|---|
| 注入物 | 一个假异常类 + 指向它的表项 | 一段**完整可执行的代码**(反射 + 响应) |
| 能否按"类"识别 | 能(`catch_type` 唯一指向) | 不能(用的全是 JDK 类) |
| 语义等价性 | 摘表项即恒等 | 整段删掉才恒等 |

所以 checks 不能用 `dasho.ControlFlow` 那种"找到注入物的专属标识再摘"的思路——它没有专属标识,`Class.forName` / `System.exit` 在正常代码里到处都是。

能用的只有两样东西:

1. **模板形状**(注入器写死的指令序列);
2. **区间结构**(注入器为了保证 fail-open 必须套的 try 包裹,以及这段代码在 CFG 上必然形成的单入口单出口区间)。

单靠 (1) 会误伤:业务代码里也可能出现 `forName` + `getMethod` + `invoke` 的反射工具方法。单靠 (2) 会误删:任何 `try { ... }` 块都是 hammock。**两层同时成立才判**,这是本 pass 的全部安全来源。

## 2. 模板判据:三条,各自对应注入器的硬约束

| 判据 | 内容 | 为什么注入器绕不开 |
|---|---|---|
| 反射三连 | `Class.forName` + `Class.getMethod` + `Method.invoke` 三条调用**同现于区间内** | 检查主体在 `Bootstrap` 的 native 侧,Java 层只能反射调用(见 [checks.md](checks.md) §"注入产物形态") |
| 噪声谓词 | `invokestatic ()I` → `dup` → 整型常量 → `imul` → `swap` → `irem` → 条件跳转,七条连排 | 这是不透明谓词模板本身。语义是 `(seed*k) % seed ≡ 0`,选路确定;换 `level`/`implementations` 只换常量与跳转方向,不换形状 |
| 响应 | 区间内出现 `System.exit` / `Runtime.halt` / `Thread.sleep` 或 `athrow` | 没有响应就没有检查——DashO 自己也会把这种配置判成 "checks were unused" 并报错退出 |

三条都**在区间内任意位置**匹配,不要求相对顺序、不要求相邻。这是刻意的:块切分与块乱序会打散相邻性,但打散不了"这些东西都在同一段里"。

噪声谓词的七条窗口是唯一一处硬编码相邻的判据。它安全的原因不是"DashO 一定这么写",而是**反方向的**:真实业务代码不会平白写一条 `x = (f() * 4) % f()` 再拿结果做分支——那个 `swap; irem` 是"用同一个值去除自己",除了制造恒零没有别的用途。

## 3. 区间判据:hammock(单入口单出口)

记方法的指令序列为 `order[0..n)`,方法末尾单独占一个槽位 `n`(即"落到方法外")。区间 `[f, X)` 是 hammock,当且仅当三条同时成立:

1. **入口唯一**:区间外没有任何边进入区间**内部** `(f, X)`。
2. **出口唯一**:区间内每条指令的后继要么仍在 `[f, X)` 内,要么恰好是 `X`。
3. **内部连通**:区间内每个位置要么从 `f` 可达,要么**本来就全局不可达**。

第 1 条规定的是 `(f, X)` 而**不是** `[f, X)` —— 允许外部边进入 `f`。这条区别是整个判据里最容易写错的地方,见 §3.3。

### 3.1 为什么要容忍死代码

DashO 的控制流注入会往方法体里撒 `goto L; nop; nop; nop; athrow; L:` 这种永不执行的填充块(与 `controlflow` 同族,见 [control-flow.md](control-flow.md))。它们物理上落在检查段内部,但**不从任何地方可达**。

如果坚持"区间内每条指令都从 `f` 可达",这两类语料的注入段会全部判不出来。所以判据改成:

```
区间内某位置不可达  ⟹  它必须全局不可达（从方法入口和所有 handler 入口都到不了）
```

"全局不可达"这个条件足够强:它排除了"从区间外某条路径能进来的活代码",而后者正是误删的唯一危险来源。计算时把**所有 handler 入口**都作为可达性根——handler 体只被异常边可达,不算进去就会把活的 catch 体判成死代码。

### 3.2 异常边必须按"全可能"加

建图时,异常边对 try 区间内的**每一条**指令都加,不判 `canThrow`。

方向是刻意选的:多算边只会让 hammock 更难成立(区间内指令多出指向 handler 的后继,`X` 之外的出口变多 → 判据更严),也就是**更容易放弃、更不容易误删**;少算边才会把活代码判成区间内、进而删掉。这里不需要精确,只需要保守。

### 3.3 入口边:必须允许进 `f`,必须拒绝进 `(f, X)`

这是踩过的坑,值得单独说。

`where="Beginning"` 的注入落在方法开头,`f = 0`,不存在入口边——按 `[f, X)` 写判据也能过。但 `where="End"` 时 DashO 的产物是:

```
0  aload_0
1  getfield x
4  istore_1          ← 原表达式的值溢写到局部变量
5  <注入段开始 f=5>
...
X  iload_1           ← 检查跑完，把值读回来
X+1 ireturn
```

此时 `f > 0`,而且**必然**有一条从 `f-1` 落下进入 `f` 的边。按 `[f, X)` 写判据会把它当成"外部入口"拒掉,结果是 `where="End"` 的注入一条也剥不掉(实测:一个 6 处注入的样本只剥掉 4 处,剩下的正是唯一一处 `End`)。

为什么允许是对的:注入段对方法状态没有贡献(它只做检查、写自己新建的局部变量),所以"跳过注入段直接到 `X`"与"跑完注入段再到 `X`"等价。`eraseBatch` 的 `kRetargetToFallthrough` 会把落进 `f` 的边改锚到 `X`,语义不变。

唯一的例外是**回边**:源位置在区间之后(`i >= X`)却跳回 `f`,说明这段是循环体,删掉会把循环变成顺序流。按"源在区间之后就当回边"拒绝——Java 字节码的布局跟随结构,真回边的源必然在后面。

四条边逐一看:

| 边的源 | 边的目标 | 判定 |
|---|---|---|
| `i < f` | `f` | 允许(入口边,删完改锚到 `X`) |
| `i < f` | `(f, X)` | **拒绝**(跳进半截注入段) |
| `i >= X` | `f` | **拒绝**(回边,说明是循环体) |
| `i >= X` | `(f, X)` | **拒绝**(同上) |

## 4. 入口从哪里来:异常表项,且必须按 handler 分组

注入段一定是整段被 `try { ... } catch (Exception) { ... }` 包住的(fail-open 要求,见 [checks.md](checks.md))。所以**区间起点就是 try 的起点**,不需要从 `Class.forName` 锚点往前扫着猜。

但直接取"锚点所在表项的 `from`"是错的。原因在 `controlflow blockSplitting`:

> 块切分会把检查段那一条 `try` 拆成几十段,每段一个表项,但**共用同一个 handler**。

只取锚点所在那一段的 `from`,起点会落到方法中段,前面的检查码留在原地,产物是"删了一半"。实测某样本的一个方法有 17 段共享同一 handler,只取单段会让起点从 0 落到 131。

正确的做法:

1. 找出所有**包含 `forName` 锚点**的表项,收集它们的 `handler`(去重)——这组 handler 就是"这条注入段的 handler 组";
2. 对每个 handler,取组内**所有**表项 `from` 的最小值作为起点候选;
3. 用这组 handler 一起算可达性根(§3.1 的第 3 条)。

### 4.1 右端候选与"必须覆盖全部锚点"

右端 `X` 的候选集合 = 全部跳转落点 + 方法末尾。取最小的合法 `X` 是错的:块切分会把一条注入段切成多个**首尾相接**的 hammock,最小右端只覆盖前半段,后半段的噪声谓词会留在原地。

所以加一条硬约束:**区间必须覆盖方法内全部 `forName` 锚点**。一条注入段里的所有反射调用属于同一次检查,不可能被合法地拆开删。

配套的两条放弃条件:

- `f == 0 && X == n`:整个方法就是注入段。删完方法体为空,没有"直通"可留,放弃(注入器不会产出这种形态,出现了说明判据算错了)。
- `n > 8192`:超大方法不做区间分析(建图 + 反复 hammock 检查是 `O(n²)` 级),直接跳过。这是性能护栏,不是判据。

## 5. 剥离:三步,顺序不能换

```cpp
//   1. 先摘异常表项（eraseBatch 把边界引用当 StillUsed）
//   2. 再销毁只被摘除项引用的边界标签（Boundary 角色保护锚点，实测 err=2）
//   3. 最后删指令（删完标签句柄失效，按内容匹配表项会落空）
```

这三步是三次失败换来的:

| 顺序 | 失败现象 | 原因 |
|---|---|---|
| 先删指令,再摘表项 | 类能导出,校验器直接拒 | 删完指令后边界标签句柄失效,按内容匹配表项匹配不上,留下指向已删 pc 的陈旧表项 |
| 先摘表项,再直接 `eraseBatch` | `EditError::StillUsed`(err=2) | 异常表的 `start`/`end`/`handler` 标签是 **Boundary 角色**,会保护自己的锚点;`cu.exc` 已经换掉了,但标签对象还活着并仍指向待删指令 |
| 先摘表项 → 销毁标签 → 删指令 | 成功 | 标签销毁后锚点解除保护,`eraseBatch` 正常通过 |

标签销毁有一条必要约束:**只销毁"不被任何存活表项引用"的标签**。同一方法里多条表项常共用同一个 handler 标签(块切分的直接后果),盲删会让存活表项指向空句柄。

### 5.1 部分重叠 = 放弃

表项与区间有三种关系:

| 关系 | 处理 |
|---|---|
| 整条落在区间内(`s >= f && t <= X && h ∈ [f, X)`) | 摘除 |
| 与区间完全不相交 | 保留 |
| 部分重叠 | **放弃整个方法**(`return false`) |

部分重叠意味着"改边界会改保护语义"——注入器不会产出这种形态,出现了说明区间算错了。这时候宁可不动。

同理,`dropped.empty()` 也放弃:一条表项都摘不到,说明判据和异常表对不上,不能留下"代码删了、表项还在"的半成品。

## 6. 死填充清扫与帧链

摘完区间后,方法里常留着控制流注入撒的死填充块。它们不执行,但**帧再生只给可达的分支目标发帧**,死块又需要帧才能过校验(classfile ≥ 50),于是产物会 `VerifyError: Expecting a stack map frame`。

所以剥离成功后追加三步:

1. `pruneUnreachable`:删掉全局不可达的指令;
2. `tidyGotoToNext`:清掉"跳到紧邻下一条"的 `goto`;
3. `foldStoreLoadPairs`:折叠 `where="End"` 留下的溢写对(§6.1)。

`pruneUnreachable` 只在**异常表已空**时做。还有真 handler 时,handler 体可能"只被异常边可达",按普通可达性算会误删——这与 [control-flow-strip-pass.md](control-flow-strip-pass.md) §3.2 的取舍同源。

改写过的方法一律按 `structural=true` 记账,走 `prepareNestedAttrsForLayout` + `regenerateFrames`(Strict 重算 → maxes 采用 → lower → SMT 再生挂回)。这里没有"保布局"分支:检查段的删除必然改变布局,保留原帧没有意义。

### 6.1 溢写对折叠

`where="End"` 的注入会把原表达式的值先溢写到局部变量,等检查跑完再读回来。注入段删掉之后,`xSTORE n; xLOAD n` 就只剩"写进去、立刻读回来",净栈效应为 0。一个一行 getter 会从

```
aload_0; getfield x; ireturn
```

变成

```
aload_0; getfield x; istore_1; iload_1; ireturn
```

语义没变,但反编译结果脏。所以剥离后再跑一遍 `foldStoreLoadPairs`,四条安全条件:

1. 两条在 `order` 里**直接相邻**;
2. **load 不是分支目标**;
3. 同族同槽(`istore_1` 配 `iload_1`,不配 `aload_1`);
4. 方法里**没有别的指令读这个槽**(含 `iinc`)。实例方法的 `this`(槽 0)通常被读很多次,自然被挡住。

第 1 条**不等于**第 2 条。`order` 只装指令,不装 label:两条指令之间夹一个 label 时,它们在 `order` 里照样相邻。真正决定安全的是有没有分支落到那个 label 上——若有一条边直接跳到这条 load,`kRetargetToFallthrough` 会把它改锚到对之后,那条路径就少了这一对"写进去、读回来"的存取。栈形不变(这一对净效应 0),但原本期望从局部变量取值的地方拿不到值,于是编译器会报 `VerifyError` 或读到脏值。所以先扫全方法的分支/switch 目标,把落点位置记进 `isTarget`,再逐对检查 `isTarget[i+1]`。

`dasho.Cleanup` 也复用同一个函数(它跑在**每个**方法上,包括带异常表的),所以这条守卫不是可选项。

## 7. 死类清扫:三路候选 + 活引用兜底

检查注入会留下两类残骸:

| 残骸 | 形态 |
|---|---|
| helper 壳 `{Owner}$N` | 无字段、无 `<init>`、全 static;一个 `(String,…)String` 解密器 + 一个 `()I` 种子发生器;方法名伪装成 `replace`/`copyValueOf`/`concat`/`regionMatches`/`startsWith`/`endsWith`/`indexOf`/`insert`/`subSequence`/`valueOf` 之类 |
| 运行时门面 `Bootstrap`(+内部类) | ≥1 个 `native` 方法;除 `<init>` 外全部 `static`;无实例字段 |

候选取三路并集:

1. `collectDecryptorClasses`:形态上是解密器壳的类;
2. `touched`:注入段里出现过的类(含被 `new`/`invokestatic` 直接引用的 helper);
3. `collectCheckRuntimeFacades`:形态上是运行时门面的类。

第 2 路补第 1 路的漏:DashO 会为入口闭包里的每个类**预生成**一份 helper,很多从未被注入段调用过。第 3 路的判据刻意宽松(真 JNI 类通常有实例 native 方法,不会命中)。

三路都只是"允许考虑"。**真正删除由 `eraseUnreferencedClasses` 的活引用检查兜底**,并且只在 `written == dirtyClasses` 时进入删除阶段——任一类写回失败就会留在原始字节上,而原始字节仍引用这些类。

## 8. 拒绝面

| 情形 | 行为 |
|---|---|
| 方法里没有 `Class.forName` | 不进入区间检测 |
| 有 `forName` 但不在任何异常表项内 | 同上(注入段必然被 try 包住) |
| 有反射三连但无噪声谓词 | 不删(可能是业务反射代码) |
| 有噪声谓词但无响应 | 不删(没有响应的配置 DashO 自己也会判 unused) |
| 区间有外部入边 | 不删(不是 hammock) |
| 区间覆盖不到全部锚点 | 不删(说明还有半段在外面) |
| 表项与区间部分重叠 | 放弃该方法 |
| 一条表项都摘不到 | 放弃该方法 |
| `eraseBatch` 失败 | **放弃整趟**,容器保持原样(不产出"改一半"的结果) |
| 方法有 `jsr`/`ret` | 不碰(子程序形态不在建模范围) |
| 标签解析失败 | 不碰该方法 |
| 方法指令数 > 8192 | 跳过 |
| 整个方法都是注入段 | 跳过(没有直通可留) |

`eraseBatch` 失败放弃**整趟**而不是单个方法,是因为此时"判据说这里是注入段"和"改写成不成立"直接冲突,方法体已不可信。本 pass 只在 `set.commit` 里落盘,所以整体放弃是干净的——容器一个字节都不会变。

## 9. 测试与回归

单测(`test/unittest/checks_pass_test.cpp`,6 例)。fixture 是手工拼的 67 字节方法体,各变体保持**逐字节相同的布局**,这样 pc 断言可以直接迁移:

```
0   invokestatic Helper.seed:()I       ; 噪声谓词
1   dup
2   iconst_1
3   imul
4   swap
5   irem
6   ifeq 19                            ; → 双支 ldc
7   ldc "cA"; …decrypt…                ; 支 A
13  goto 22
16  ldc "cB"; …decrypt…                ; 支 B（解密后同值）
22  invokestatic Class.forName         ; 反射三连
33  invokevirtual Class.getMethod
43  invokevirtual Method.invoke
52  pop
53  goto 64
55  pop; bipush -5; System.exit        ; handler（响应）
64  ldc "ok"; areturn                  ; 原逻辑
```

异常表:`{start=0, end=55, handler=55, catch=java/lang/Exception}`。

| 用例 | 断言 |
|---|---|
| `StripsInjectedSegmentAndSweepsHelper` | 区间 `[0, 64)` 整段删除,方法只剩 `ldc "ok"; areturn`,异常表清空,helper 类被扫掉 |
| `KeepsReflectionWithoutNoisePredicate` | 三连 + 响应都在,只缺噪声谓词 → 完全不动 |
| `KeepsNoiseWithoutResponse` | 噪声谓词 + 三连都在,只缺响应 → 完全不动 |
| `KeepsRegionWithExternalEntry` | 区间**内部**有外部入口(方法尾 `goto 22`) → 不是 hammock → 完全不动 |
| `StripsSegmentWithFallthroughEntry` | `where="End"` 形态:`ldc "ok"; astore_1` 序言 + 区间 `[3, 67)` + `aload_1; areturn` 收尾 → 整段删除**且**溢写对被折叠,只剩 `ldc "ok"; areturn` |
| `KeepsHelperStillReferencedByLiveCode` | 剥离成功,但 helper 仍被活代码引用 → 只删区间,不删类 |

### 9.1 语料回归

`test/corpus-regress.sh`,四通道联跑 `Checks,ControlFlow,StringDecrypt`(+ 有 `.map` 时附 `Rename`),29 个样本 jar:

- **0 CLI 失败、0 结构损坏、0 类数增长**;
- 只有 3 个 checks 样本命中,其余 26 个全部 no-op —— **判据不误报**;
- 输出条目集与**未混淆的原 jar 完全一致**,JDK `Class.forName` 校验全绿;
- `Checks→ControlFlow→StringDecrypt→Rename` 与 `ControlFlow→Checks→StringDecrypt→Rename` **两种顺序都成立**(checks 段的定位不依赖 controlflow 是否已经跑过);
- 还原 jar 与原 jar 的启动栈迹逐行一致。

### 9.2 多 check 样本:判据的普适性

单一 `debuggingCheck` 样本只能证明"这一种 check 剥得掉"。为验证判据是**通解**而不是拟合了一个样本,另建了一个 6 处注入的样本,覆盖 2 种 check × 3 种 `where` × 4 种 `response`:

| 方法 | check | where | response |
|---|---|---|---|
| `Main.main` | `debuggingCheck` | Beginning | exit |
| `KeyboardListener.keyPressed` | `debugEnabledCheck` | Beginning | hang |
| `SquarePanel.ChangeColor` | `debuggingCheck` | BeginningAndEnd | exception |
| `ThreadsController.pauser` | `debuggingCheck` | BeginningAndEnd | error |
| `Tuple.getX` | `debugEnabledCheck` | End | exit |
| `ThreadsController.run` | `debugEnabledCheck` | End | exception |

结果:注入段与 helper 全部清除,类数与原 jar 一致,逐类加载校验通过。

这个样本抓出了两个真实缺陷(都是单样本时看不出来的):

1. **hammock 拒掉了 `where="End"`** —— 第一版漏剥了唯一的 `End` 注入。修法见 §3.3。
2. **溢写对残留** —— `Tuple.getX` 还原成 `getfield; istore_1; iload_1; ireturn`,多一对存取。修法见 §6.1。

### 9.3 已知残留差异(非本 pass 引入)

| 差异 | 来源 |
|---|---|
| 方法保留 DashO 的 `nop` 填充 | 信息论层面不可还原(原指令已不存在),语义等价 |
| 局部变量槽位重编号 | **DashO 自身**的产物,三方对比(原 jar / 混淆 jar / 还原 jar)证明混淆 jar 里就已经是重编号后的槽位 |
| switch `default` 目标因填充偏移而位移 | 同上,布局副作用 |
| 包私有成员被放宽为 `public` | `renaming` 的既有行为,见 [rename-pass.md](rename-pass.md) |

## 10. 与 `dasho.ControlFlow` 的分工

两者都处理"注入的 try 块",但入口判据完全不同,不构成竞争:

| | `dasho.ControlFlow` | `dasho.Checks` |
|---|---|---|
| 锚点 | 假异常类的 `catch_type` | `Class.forName` 指令 |
| 动作 | 只摘表项(+ 表空时清死码) | 删整段代码 + 摘表项 |
| 对 checks 注入 | **no-op**(catch 类型是 `java/lang/Exception`,不是生成的假异常类) | 命中 |
| 对 tryCatch 注入 | 命中 | **no-op**(没有反射三连 + 噪声谓词) |

两者互不误伤,顺序无关。这一点在语料回归里双向验证过(§9)。
