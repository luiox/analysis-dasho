# DashO 控制流混淆(Control Flow)技术档案

本文描述 DashO 12.8 控制流混淆(controlflow)的**通用技术形态**:配置面、三个子功能各自的产出差异、tryCatch 假异常类注入形态、blockSplitting/jumbling 块变换形态,以及各类形态的剥离判据。

## 1. 配置面(dasho.xsd ControlflowType)

```xml
<controlflow option="on"
             tryCatch="on|off"           <!-- 默认 on -->
             catchHandlers="1..10"       <!-- 默认 1,每方法最大注入 handler 数 -->
             blockSplitting="on|off"     <!-- 默认 on -->
             blockSplittingBlockSize="1..32767"  <!-- 默认 3,最小块大小 -->
             jumbling="on|off"           <!-- 默认 on,块乱序 -->
/>
```

## 2. 三个子功能的产出差异(通用)

| 子功能 | 新增类 | 代码形态变化 |
|---|---|---|
| `tryCatch` | **每个受控类 +1** 假异常类 | 方法体被包进 try 区间 + 不可达 `athrow`/`nop` 填充 |
| `blockSplitting` | 0 | 基本块切到最小 N 条指令,块间插 `goto` 接力 |
| `jumbling` | 0 | 块序与语义序解耦,无 fall-through |

三者可独立开关。**全开的类数增量只由 tryCatch 决定**(切块/乱序不产生类),代码同时承受切块 + 乱序 + 异常表注入。这条是判断"某产物开了哪些子功能"的第一层分流:类数增长 → tryCatch 开;类数不变但 CFG 变形 → 只有 blocks/jumbling。

## 3. tryCatch 形态

### 3.1 假异常类指纹

每个受控类旁注入一个 **0 方法 / 0 字段**的 `RuntimeException` 子类:

| 属性 | 值 |
|---|---|
| 类名 | `{Owner}$<Name>`,owner 为被注入类 |
| `<Name>` 取值池 | `Exception` / `ParseException` / `ArrayOutOfBoundsException` / `NullPointerException` |
| super | `java/lang/RuntimeException` |
| access | `1`(public) |
| **classfile version** | **45.3**(major 45 / minor 3,即 Java 1.1) |
| 字段/方法 | 0 / 0 |
| 类属性 | 0(无 SourceFile) |
| 体积 | 约 76 字节 |

**版本戳是首选判据**:用户类通常 major 52–61(Java 8–17),`45.3` 这个组合不会由 javac 产出。

> **数值口径坑**:classfile 头部是 `magic u32 / minor u2 / major u2`——先 minor 后 major。把这两个 u2 按文件顺序读成一个 u32 会得到 **196653**(= `3<<16 | 45`),按 libjbc 的 `ClassUnit::version` 口径 `(major<<16)|minor` 打包则是 **2949123**。两者是同一个版本号的两种读法,**196653 不是 major 48 / minor 29**(`0x3002D` 拆成 48.29 是把它当成了 `(major<<16)|minor`)。写判据时先确认口径,否则会去匹配一个根本不存在的版本。

### 3.2 字节码注入模式

```
exceptions: { { L1, L7, L8, {Owner}$Exception } }   // 假 handler
code:
L0:
    goto L1                    // 跳过死代码
    athrow                     // 死 athrow
    nop
    nop
    athrow                     // 死 athrow
    athrow
L1:
    // ===== 真实业务代码 =====
    ...
    return
L7:
    nop                        // try 块末尾死代码
    nop
    athrow                     // 死 athrow
    athrow
    nop
    nop
    athrow
    athrow
L8:
    astore 0                   // handler:吞异常
    return
```

要点:

1. **死 `athrow`/`nop` 填充**:入口(L0→L1)和 try 末尾(L7)各塞一段不可达 `athrow`+`nop`,破坏线性扫描反编译器。
2. **假异常表项**:`catch_type` 指向 `{Owner}$<Name>` 假类,handler 体仅 `astore + return`(吞异常,永不执行——因为死 `athrow` 不可达)。
3. **`catchHandlers=N`** 是每方法上限,实际注入数受方法体量与可切分位置限制,通常远小于 N。
4. 死填充段的指令序列不固定(段内 `nop`/`athrow` 数量会变),但**结构固定**:`goto` 跳过一段只含 `nop`/`athrow` 的不可达块。

### 3.3 剥离判据(tryCatch)

```
识别:
  A. 异常表项 catch_type 指向 version==196653 且 0 方法的 RuntimeException 子类
  B. handler 体仅 astore_* + return(或 athrow)
  C. try 范围内含不可达 athrow/nop 填充段
剥离:
  1. 删除该异常表项
  2. 删除 try 范围内死 athrow/nop 填充(goto 可直达真代码)
  3. 删除 handler 死代码
  4. (可选)删除 0 方法假异常类
```

### 3.4 与真 handler 共存(剥离的硬约束)

- **注入只加项、不改项**:方法原有的真 try-catch(如 `try { sleep } catch (InterruptedException)`)原封不动,注入项与它并列在同一张异常表里。所以"异常表里有项"完全不能作为"被注入过"的判据,必须看 `catch_type` 指向谁。
- **注入项的 try 区间不连续**:块切分之后一个方法会拿到 2–3 段区间(例如 `[8,33)` 与 `[38,187)`),全部指向同一个 handler;段与段之间的空洞是被切出去的块。因此"从 goto 到 handler 是一条连续区间"这种形态假设不成立。
- **handler 体只吞不抛**:`astore_* ; return`,没有 `athrow` 也没有 `throw`。这是"注入项"的第二特征(第一特征是 `catch_type`),但只有在类被 renaming 改掉、版本戳又被抹平时才需要用到。
- 由此推出剥离的硬约束:**只要方法还留着真 handler,就不能重算帧**——Strict 域的异常边只从可抛指令发出,算出的 handler 帧比 verifier 的保守合并(整个 try 区间每条指令的 locals 全量合并)更精确,写回即 `VerifyError`。这条约束决定了改写策略的分工,见 [control-flow-strip-pass.md](control-flow-strip-pass.md)。

## 4. blockSplitting + jumbling 形态
### 4.1 块乱序(jumbling)

原顺序代码被完全打散:

```
L0 → goto L5          // 入口跳到中间
L1 → 比较 → goto L32  // 远跳
L2 → getY → goto L14
...
L32 → 末尾块
L35 → return
L36 → 补块
```

无任何 fall-through;每块以 `goto` 结束,块序与语义序无关。`tableswitch`/`lookupswitch` 保留,但 case 目标也是乱序块。

### 4.2 块切分(blockSplitting)

`blockSplittingBlockSize=3` 使基本块最小 3 条指令。切分后:

- 原一个语义块拆成多个 3–5 指令小块;
- 每块间插 `goto` 接力(大量短 `goto` 紧邻)。

### 4.3 不透明谓词填充(与字符串加密共用模板)

反复出现的**5 指令死代码序列**:

```
ldc -1
istore N          // 存 -1
ldc "0"           // 死加载字符串
iinc N 1          // 死增量
astore M          // 死存储
goto next         // 恒定跳转
```

- `ldc "0"` / `iinc` / `astore` 是纯垃圾——不参与控制流,仅混淆 CFG 与常量池;
- 与字符串加密的 `()I` 种子谓词**同源不同形**:这里是静态死代码,字符串加密是 `Random.nextInt` 动态不透明谓词;
- 剥离:识别 `ldc -1; istore; ldc "0"; iinc; astore; goto` 模板,整段删除;
- 边界:动态不透明谓词(字符串调用点的种子 + 双支 ldc)由字符串加密还原前置处理,判据见 [string-encryption.md](string-encryption.md) §3——**不入本节剥离清单**,本节仅静态形态。

### 4.4 剥离判据(blocks/jumbling)

**实测结论:块序重排不需要做,死填充必须删。** 两者分开看:

| 目标 | 结论 | 依据 |
|---|---|---|
| 恢复语句顺序 | **不做** | 块序对反编译器是冗余信息——CFR 打 `Enabled aggressive block sorting` 自行重建;Vineflower 连 CFR 都还原不了的 jumbled `Window.<init>`(嵌套 for)都能完整还原 |
| 删死填充 / goto 接力 | **做** | 反编译器会把填充当成真实语句输出(`int n = -1; ++n; String string = "0";`),`javap` 层也全是噪声 |

重排块序的代价不小:改块序会让异常表 `[start, end)` 不再连续,必须拆分/合并 try 范围,而收益只体现在 `javap` 的线性阅读上。所以本仓的实现(`dasho.Cleanup`,见 [cleanup-pass.md](cleanup-pass.md))只做删除,不动顺序:

```
识别(空转窗口,四条同时成立):
  A. 栈自足:深度从 0 起全程非负、收在 goto 前回到 0
  B. 窗口写下的局部变量槽在出口之后不活跃(逐指令活跃性分析)
  C. 收在 goto 前,且目标不在窗口内
  D. 窗口内部没有分支目标
剥离:
  1. eraseBatch(窗口, kRetargetToFallthrough)  → 窗口退化成一条孤立 goto
  2. threadGotos                                → 指向它的边接到终点
  3. pruneUnreachable                           → 收走孤立 goto 与死填充
  4. tidyGotoToNext + foldStoreLoadPairs        → 清 goto→next 与溢写对
```

判据为什么是这四条、为什么"净效应 0"不够、为什么必须用活跃性而不是"全方法不读",见 [cleanup-pass.md](cleanup-pass.md) §3。

`line number 与块语义序错位`(识别项 D)在本轮样本里**没有出现**——DashO 保留了原始行号,只是把它挂在被搬走的块上。真要按行号排序,那是"重排"而不是"删除",按上面的结论不做。

## 5. 与 Allatori 对照

| 维度 | Allatori 6.x/8.x/9.x | DashO 12.8 |
|------|---------------------|------------|
| 块乱序 | 有(FlowObfuscation) | 有(jumbling) |
| 块切分 | 有(minBlockSize) | 有(blockSplittingBlockSize) |
| tryCatch 注入 | **未见独立假异常类** | **有**(version 196653 指纹) |
| 不透明谓词 | 有(多种模板) | 有(ldc -1/istore/ldc "0"/iinc 模板) |
| 死 athrow 填充 | 未见明确 | **有**(tryCatch 专用) |

DashO 的 tryCatch 假异常类模式是**独有指纹**——可据此精确识别。更多维度的对照见 [allatori-comparison.md](allatori-comparison.md)。
