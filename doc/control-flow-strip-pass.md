# `dasho.ControlFlow`:通解控制流注入剥离 pass 的设计

本文是 `dasho.ControlFlow` 的实现说明:**判据为什么落在 catch_type 而不是形态**、两条改写策略如何按"方法还剩不剩真 handler"分工、以及可达性分析为什么是整个 pass 里唯一危险的操作。技术形态本身见 [control-flow.md](control-flow.md)。

## 1. 设计立场:认注入物,不认注入形状

`controlflow tryCatch` 的产物形态会随方法体量、块切分位置、`catchHandlers` 取值变化:头部填充段的 `nop`/`athrow` 条数不固定、try 区间可能被切成 2–3 段、注入点可能不在方法头部(块乱序时 switch 先出现)。按"方法开头有个 goto 跳过一段 athrow"去匹配,样本一换就漏。

注入无法伪装的是**它必须引入的东西**:

| 判据 | 内容 | 为什么伪装不掉 |
|---|---|---|
| 假异常类 | classfile **45.3** + 0 字段 + 0 方法 + super=`RuntimeException` | DashO 生成类走它自己的写出路径,版本戳恒 45.3;假异常类要能被 `athrow` 捕获就必须是 Throwable 子类,而它从不被实例化,所以不可能有成员 |
| 异常表项 | `catch_type` 指向上述类 | `catch_type` 是结构性引用,改它就等于改捕获语义 |

只要这两条成立,handler 就**永不可能触发**:它的捕获类型是一个从不被 `new` 的类。摘掉这条表项,程序行为逐位不变——这不是"近似还原",是语义恒等。

## 2. 为什么必须按 catch_type,不能按"异常表里有项"

注入是**加项**,不是**改项**。方法原有的真 try-catch(`try { sleep } catch (InterruptedException)`)原封不动,和注入项并列在同一张表里。实测样本里 `ThreadsController.pauser` 就是这样:真 handler 与注入项共存于同一个方法。

所以"有异常表 ⇒ 被注入过"是错的,"异常表为空 ⇒ 没被注入过"也是错的(注入项可能落在别的方法)。唯一可靠的入口是逐项解析 `catch_type`。

## 3. 两条改写策略

分工判据只有一句:**摘完之后异常表还空不空**。它直接对应"帧还能不能重算"。

### 3.1 结构改写(异常表被摘空)

表空了就没有 handler,也就没有 verifier 要求的**保守 handler 帧**——帧重算的唯一障碍消失。于是可以放开手删:

1. 从方法入口(下标 0)求可达指令集;
2. 删掉全部不可达指令 —— 头部填充段、尾部填充段、handler 体一次删完;
3. 清掉"跳到紧邻下一条"的 `goto`(注入在头部时必然留一条,填充段删掉后它就是纯噪声);
4. 写回时按新布局再生 StackMapTable。

第 3 步是纯粹的产物洁净度:留着它反编译器会为一个空跳转造块,删掉它对语义零影响。用 `kRetargetToFallthrough` 删除——若还有别的分支跳这条 `goto`,先把它们改锚到下一条。

### 3.2 保布局改写(方法还有真 handler)

只摘异常表项,**一条指令都不动**。指令数与栈形不变,既有帧(含真 handler 的保守帧)逐条仍然成立,由 lower 的 relocation 精确搬运。

为什么不能顺手把填充段也删了:删指令 ⇒ 布局变 ⇒ 必须重算帧 ⇒ Strict 域的异常边只从 `canThrow` 指令发出,算出的 handler 帧比 verifier 的保守合并更精确 ⇒ 写回即 `VerifyError`。这与字符串 pass 的取舍同源(见 [string-decrypt-pass.md](string-decrypt-pass.md) §3.2),代价也一样:填充段留在原地,是可读性上的噪声而非正确性问题。

## 4. 可达性:本 pass 唯一危险的操作

摘表项是安全的(语义恒等),删假异常类是安全的(删前有引用检查),**删指令不是**——判错一条就是删掉活代码,程序直接坏掉。所以可达性分析全程 fail-closed:

| 不确定的情形 | 处理 |
|---|---|
| 标签解析失败(`resolveLabel` 报错) | 放弃该方法的结构改写,退回保布局 |
| 出现 `jsr`/`jsr_w`/`ret` | 同上(子程序形态的返回地址集合不在本 pass 建模范围) |
| `eraseBatch` 报 `StillUsed` | 同上(说明还有存活引用指向待删指令,说明可达性算错了) |
| 异常表边界落在方法末尾 | 正常:`end` 标签解析为 `order.size()`,按右开区间处理 |

可达性本身的三条边:

- **顺序边**:除无条件跳转/switch/return/athrow 外,每条指令都能落到下一条;
- **跳转边**:`target`(含 switch 的 default)与 `targets[i]`;
- **异常边**:try 区间内只要有一条可达指令,handler 就可达。

异常边必须在**摘完注入项之后**算:注入 handler 还在表里时,它会把自己的 handler 体标成可达,死填充就永远删不掉。

## 5. 假异常类的删除条件

判据命中不等于可以删。类被删了但还有东西引用它,产物就是坏的。三道门全过才删:

1. **写回全部成功**:任一类写回失败就会留在原始字节上,而原始异常表仍引用这些类。用一个保守条件挡住——`written == dirtyClasses` 才进入删除阶段。
2. **没有"保布局"的方法还引用它**:这类方法的帧没重算,handler 帧的栈顶还写着假异常类。按方法记账:哪个方法走了保布局路径,它摘掉的 `catch_type` 就进 keepAlive。
3. **没有指令显式引用它**:扫全部指令的 `new`/`checkcast`/`anewarray`/`instanceof`/`multianewarray` 与 `ldc` 的 Class 常量。注入产物不会 `new` 自己的假异常类,但用户代码里可能有同名巧合,或者样本被二次处理过。

常量池里的残留 `Class`/`Utf8` 条目不算引用:CP 条目不会强制加载类,留着无害,压缩留给 CP compact。

## 6. 写回与帧链

复用 `ClassSet` 的既有链路,与字符串 pass 完全同构:

- `DirtyMethod{idx, structural}`,只有 `structural=true` 的方法才走
  `prepareNestedAttrsForLayout` + `regenerateFrames`(Strict 重算 → maxes 采用 → lower → SMT 再生挂回);
- `structural=false` 的方法只 lower:异常表变了会被 `lowerMethod` 判脏(它比对 `exc` 表),指令布局未变所以 pcmap 是恒等映射,LNT/LVT/SMT 的 relocation 精确且无损;
- `major < 50` 的方法不产 StackMapTable,`regenerateFrames` 直接返回 true,maxes 沿用原值(只删代码,原 maxes 仍是上界)。

## 7. 拒绝面

| 情形 | 行为 |
|---|---|
| jar 里没有假异常类 | 整体 no-op,返回 `PreservedAnalyses::all()` |
| 假异常类存在但没有任何表项指向它 | 同上(不删孤立类:没有证据表明它是本 pass 该动的东西) |
| 版本戳 45.3 但类有成员 | 不判为假异常类(判据三重条件缺一不可) |
| 方法有 `jsr`/`ret` | 只摘表项,不删指令 |
| 结构改写失败 | 降级为保布局改写,并把该类的假异常类标 keepAlive |
| 任一类写回失败 | 不删任何假异常类(留全尸,便于对比排查) |

## 8. 测试与回归

单测(`test/unittest/control_flow_pass_test.cpp`,5 例)覆盖:

- 注入项是唯一表项 → 填充段、handler、头部 `goto` 全部消失,假异常类被删;
- 真 handler 共存 → 只摘注入项,指令条数与 `athrow` 数不变,假异常类保留;
- 只有真 handler → 完全 no-op;
- 版本戳 45.3 但带成员 → 不判为假异常类;
- 假异常类被 `new` 引用 → 表项照摘,类保留。

语料回归(`run-lab` 22 个 jar,`dasho.StringDecrypt` + `dasho.ControlFlow` 联跑):

| 样本 | 结果 |
|---|---|
| `cf-trycatch` | 22 项 → 14 个方法结构改写,7 个假异常类删除,8650B → 6442B |
| `cf-full` | 7 项 → 5 个方法结构改写,7 个假异常类删除 |
| `cf-blocks` | no-op(只开切块/乱序,不注入 tryCatch)—— 判据不误报 |
| `checks-debug` | no-op(checks 注入不产假异常类)—— 判据不误报 |
| 其余 18 个 jar | no-op |
| 全部 22 个 jar | JDK `Class.forName` 校验 0 失败;`cf-*` 的 headless 栈迹与原 jar 逐行一致 |

`cf-trycatch` 的 `pauser`(真 handler)在产物里逐字节保持原样,`moveInterne`(tableswitch 开头、注入点在 pc 32)的 case 目标全部正确重映射。
