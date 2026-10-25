# `dasho.Cleanup` — 控制流填充的清除

> 对应实现：`src/dasho/cleanup_pass.{hpp,cpp}`，共用底座 `src/dasho/cfg.hpp`。
> 技术形态见 [control-flow.md](control-flow.md) §4。

## 1. 它解决什么

`dasho.StringDecrypt` / `ControlFlow` / `Checks` 三个 pass 都把"有语义的混淆"还原了，但方法体里还剩一类**不改变程序状态、只破坏线性扫描**的代码。三类来源、同一性质：

| 来源 | 形态 | 性质 |
|---|---|---|
| `controlflow blockSplitting` / `jumbling` | `ldc -1; istore N; ldc "0"; iinc N 1; astore N+1; goto L` | 栈自足、只写从不被读的局部变量 |
| `controlflow tryCatch` | `goto L; nop; nop; athrow; L:` | 全局不可达 |
| 块切分本身 | 大量 `goto` 接力、`goto` 到紧邻下一条 | 控制流冗余 |

它们对**运行**没有影响，但对**阅读**影响很大：反编译器会把填充当成真实语句。

```
// 混淆后（CFR，未清理）            // 清理后
public static void main(...) {      public static void main(...) {
    int n = -1;                         Window window = new Window();
    ++n;                                window.setTitle("Snake");
    String string = "0";                window.setSize(300, 300);
    Window window = new Window();       window.setVisible(true);
    window.setTitle("Snake");           window.setDefaultCloseOperation(3);
    ...                             }
}
```

## 2. 先证伪：块重排没有必要

`doc/control-flow.md` §4.4 的原始计划里有一条"重排 block 顺序 + 修正 line number"。实测**不需要**，理由是两个反编译器都自己做块重排：

- CFR 会打 `Enabled aggressive block sorting`，按 CFG 重建语句顺序；
- Vineflower 更彻底：jumbled 的 `Window.<init>`（嵌套 for 循环，`goto` 乱飞）在 CFR 里报 `Unable to fully structure code`，Vineflower 却能完整还原成嵌套 `for`。

也就是说**块序信息对反编译器是冗余的**（CFG 已经决定了语义），重排只能改善 `javap` 的线性阅读。而代价是：改块序会让异常表 `[start, end)` 区间不再连续，必须拆分/合并 try 范围——高风险、低收益。

所以本 pass **不动块序**，只做"删除无语义贡献的代码"这一类确定安全的改写。

## 3. 空转窗口判据

记方法的指令序列为 `order[0..n)`。一段连续窗口 `[i, j)` 可删，当且仅当：

| # | 条件 | 为什么 |
|---|---|---|
| 1 | **栈自足**：从入口栈深 0 起逐条推演，深度全程非负且终值为 0 | 见 §3.1 |
| 2 | **写下的槽在出口已死**：窗口写到的每个局部变量槽，在 `order[j]` 处不活跃 | 见 §3.2 |
| 3 | **收在 goto 前**：`order[j]` 是 `goto`，且其目标不在 `[i, j]` 内 | 单出口；跳回窗口内说明不是"执行完就走" |
| 4 | **内部无块首**：`order[i+1..j]` 里没有分支目标 | 否则有边能跳进窗口中间，删了会悬垂 |

窗口**不必是基本块**。DashO 会把填充插在实参求值中间：

```
aload_0                 // this
getstatic directionSnake// 实参
ldc -1                  // ← 填充起点，从中间插入
istore_1
ldc "0"
iinc 1, 1
astore_2
goto 26                 // 跳到 moveInterne(directionSnake)
```

按"整块"判会漏掉这一类（整块的栈深从 0 起算不为 0），只有窗口级判据能覆盖。

### 3.1 为什么"净效应 0"不够

`pop; aconst_null` 的净效应也是 0，但它把入口栈顶的值**换成了 null**。删掉它，后面的代码拿到的就不是 null 了。

所以判据是"逐条推演、深度全程非负"——深度一旦为负，说明这条指令消费了窗口外的值，窗口不是自足的。这条同时排除了 `swap`、`dup_x1` 这类需要更深的入口栈才能成立的形态。

### 3.2 为什么用活跃性而不是"全方法不读"

DashO 的填充**会复用真实变量的槽**。`ThreadsController.checkCollision` 里填充写槽 4，而槽 4 正是真循环变量——按"这个槽全方法都不被读"判会一条也删不掉。

正确的判据是标准的 dead-store：窗口写下的槽在窗口出口**之后**不活跃（`liveIn[order[j]]` 里没有它）。实现是逐指令的反向数据流：

```
liveIn[i]  = use(i) ∪ (liveOut[i] \ def(i))
liveOut[i] = ∪ liveIn[succ(i)]
```

`long`/`double` 占两个槽，读写都按跨度标记（读半边也算读）。求解到不动点，`n + 2` 轮封顶。

### 3.3 只处理没有异常表的方法

有真 handler 时，删掉一条指令就可能让 handler 体变成"只被异常边可达"，而帧再生只给可达目标发帧——留下死块即 `VerifyError: Expecting a stack map frame`。这类方法**整体跳过**，一个字节都不动（实测 `ThreadsController.pauser` 被跳过，输出与原 jar 语义一致）。

## 4. 改写怎么落地

不用"把 A 的所有前驱改指 B"这种要自己维护边表的动作——库没有这个原语。用三步原子操作组合：

```
1. eraseBatch(窗口指令, kRetargetToFallthrough)
      → 跳向窗口首的边被改锚到窗口之后的第一个存活指令，也就是那条 goto
      → 窗口位置退化成一条孤立 goto
2. threadGotos
      → 顺着 goto 链走到头，把指向它的边一次性接到终点
      → 那条孤立 goto 从此没有前驱
3. pruneUnreachable
      → 删掉全局不可达的指令（含那条孤立 goto）
```

`kRetargetToFallthrough` 是这一步能成立的关键：它天然把"跳向已删区间"的边接到区间之后，正好是我们要的落点。异常表边界引用会按 `StillUsed` 拒绝——但本 pass 只在 `exc` 为空的方法上跑，不会撞上。

### 4.1 goto 线程化

块切分把一条长跳拆成 `goto A` / `A: goto B` 这样的接力。`threadGotos` 顺着链走到头再一次性改写，比逐跳改快，也不会在 `goto A` / `A: goto B` / `B: goto A` 这种环上来回震荡（visited 集在第一次重复处停）。

switch 的 case 表不动：`setTarget` 只改默认目标，改了会让默认分支和 case 分支指向不同的中转块，形态更差；反正两边最终落到同一处，不改也不影响语义。

### 4.2 溢写对折叠

复用 `cfg.hpp` 的 `foldStoreLoadPairs`（`where="End"` 的检查注入留下的 `xSTORE n; xLOAD n`）。放在最后跑：前面几步把中间的 goto / 死块清掉之后，原本被隔开的一对才浮出来。

### 4.3 迭代

```
for round in 0..3:
    pruneUnreachable  +  bypassNeutralRuns  +  threadGotos  +  tidyGotoToNext
    if 本轮无改动: break
foldStoreLoadPairs
```

几步互相解锁（删死块让 goto 相接、旁路让新死块出现），实测两轮收敛。

## 5. 拒绝面

| 形态 | 结果 | 原因 |
|---|---|---|
| 窗口写到的槽在出口活跃 | 不动 | dead-store 判据不成立 |
| 窗口消费入口栈（`pop; aconst_null`） | 不动 | 不是自足窗口 |
| 窗口内有分支目标 | 不动 | 删了会让那条边悬垂 |
| `goto` 跳回窗口内（自环 / 内跳） | 不动 | 不是单出口 |
| 指令不在栈效应白名单（`invoke*` / `getfield` / `idiv` / 数组读写 / `checkcast`） | 不动 | 可能抛异常或有副作用 |
| `ldc` 的是 Class / MethodType / MethodHandle / Dynamic 常量 | 不动 | 首次解析可能触发类加载 |
| 方法有异常表 | 整个方法不动 | 帧再生不给死块发帧 |
| `jsr`/`ret`、标签解析失败 | 整个方法不动 | 可达性算不出来 |

方向始终是"宁可漏清，不可误删"。

## 6. 测试与回归

单测 `test/unittest/cleanup_pass_test.cpp`（6 例），夹具手工拼五种产物形态：

| 用例 | 断言 |
|---|---|
| `BypassesDeadStoreFill` | 填充整段删除 + `goto` 到紧邻下一条被清 → 只剩 `ldc; areturn` |
| `KeepsFillWritingLiveSlot` | 填充写到的槽在出口仍被读 → 填充原样保留 |
| `KeepsWindowConsumingEntryStack` | `pop; aconst_null` 净 0 但吃入口栈 → 不动 |
| `PrunesUnreachableFill` | `goto L; nop; athrow; L:` 死块被剪除 |
| `SkipsMethodWithExceptionTable` | 带异常表 → 一个字节不动 |
| `KeepsWindowWithBackEdgeGoto` | `goto` 跳回窗口首 → 不动（删了变自环） |

语料回归 `test/corpus-regress.sh`（默认链含 `dasho.Cleanup`，**新增逐类加载校验**）：全语料 0 失败、0 类数增长、0 校验失败。
