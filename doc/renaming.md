# DashO 重命名(Renaming)技术档案

本文描述 DashO 12.8 重命名(renaming)的**通用技术形态**:配置面、生成名的分配规则、自定义字母表带来的判读陷阱、入口点与特殊成员保留规则、包扁平化、mapfile 双格式与还原口径。

## 配置面(dasho.xsd RenamingType)

- `rename`(bool):全局开关。
- `alphabet`:类名/成员名生成字母表(预定义或自定义字符串)。
- `minlength`(int):新名最小长度。
- `randomize`(bool):每次构建名字不同(增量混淆需 mapfile)。
- `keeppackages` / `keeppublics`:保留包结构 / 保留 public 名。
- `mapping(mapinput/mapoutput)` / `proguardMap`:映射文件读写(兼容 ProGuard)。
- `entryPointRename`:入口点是否参与改名。
- `renameAnnotations`:内部定义注解是否改名。
- `overload induction`(UI 选项):不同签名方法共用新名(仅当签名不同)。

## 官方文档要点

- 类/方法/字段/注解/包全覆盖;**字母表可自定义**——DashO 的 i/I 类名可来自自定义字母表 `{i,I}` + minlength 配置,不是硬编码特征(与 Allatori 默认行为不同)。
- 包扁平化(默认)或保留 + class prefix(带 `.` 即换包)。
- 入口点(main 及宿主类)需 entrypoint 标注或 exclude,否则改名破坏运行。
- 反射:可自动排除反射类,或改写字符串常量中的类名("Rename Reflected Classes");**无法检测方法/字段的反射访问**——文档建议手动 entrypoint/exclude。
- 产出 renaming mapfile:解码混淆后 stack trace(ConvertMap/Lucidator),增量混淆输入;兼容 ProGuard map 格式。
- 简单重命名生成名不主动制造新 overload;overload induction 才会。

## 生成名的分配规则(通用)

### 名字序列 = 字母表上的双射 N 进制

生成名不是"字母递增"这么含糊的规则,而是**把序号写成双射 N 进制(N = 字母表长度),再逐位映射到字母表字符串上**,高位在前、最低位在末尾。默认字母表 `a…z` 时 `0→a, 25→z, 26→aa, 27→ab`。

用两个自定义字母表做正交验证(同一 jar,只改 `alphabet`):

| 序号 | `alphabet="xy"` | `alphabet="iJ"` |
|---|---|---|
| 0 | `x` | `i` |
| 1 | `y` | `J` |
| 2 | `xx` | `ii` |
| 3 | `xy` | `iJ` |
| 4 | `yx` | `Ji` |
| 5 | `yy` | `JJ` |

两张表逐位对齐 ⇒ 字母表是**字符串**(按 `alphabet[digit]` 取字符),不是字符码区间;`minlength` 只是在短名前面补位,不改变计数序列。

### 类的分配序 = 原名升序

类的取名顺序**不是** jar 内的出现序,而是**原类名的字节序升序**(`D < K < S < T < T < W`)。同一 jar 的六次分配在四种字母表下完全一致:`DataOfSquare, KeyboardListener, SquarePanel, ThreadsController, Tuple, Window` → 序号 0…5。mapfile 里的段头顺序(原 jar 出现序)与取名顺序无关,别拿段头序当计数器序。

未改名的类(入口点、`keeppackages` 下被保留的类)不占序号。

### 成员:方法/字段两个独立计数器,按类重置

每个类内部**方法**与**字段**各自从序号 0 起算,互不共享、也不跨类延续。`ThreadsController` 的字段拿到 `a…g`(7 个)时,它的方法只走到 `f`(6 个),两条线各自计数。

`<init>`/`<clinit>`、接口实现方法、`serialVersionUID`、`keeppublics` 保留的 public 成员**都不占序号**。

### 方法的 overload induction:贪心首次命中

方法名允许复用——这是 DashO 的 overload induction(默认开),也是**新名不是单射**的根因。规则是:按声明序逐方法,从序号 0 开始扫,取第一个"同名的现有方法参数列表都不同"的名字;命中已有序号则复用且**不推进计数器**,命中新序号才推进。

`ThreadsController`(默认字母表)逐方法复现:

| 方法 | 参数列表 | 取名 | 说明 |
|---|---|---|---|
| `pauser` | `()` | `a` | 序号 0 |
| `checkCollision` | `()` | `b` | `a()` 已被占,序号 1 |
| `stopTheGame` | `()` | `c` | 序号 2 |
| `spawnFood` | `(LTuple;)` | `a` | `a(LTuple;)` 空位 → 复用,不推进 |
| `getValAleaNotInSnake` | `()` | `d` | `a()/b()/c()` 都占,序号 3 |
| `moveInterne` | `(I)` | `a` | `a(I)` 空位 → 复用 |
| `moveExterne` | `()` | `e` | 序号 4 |
| `deleteTail` | `()` | `f` | 序号 5 |

判据是**参数列表**,不含返回类型:`a()V` 与 `a()LTuple;` 在 Java 重载意义上冲突,所以 `getValAleaNotInSnake()LTuple;` 不能复用 `a`,只能拿 `d`。字段没有重载概念,因此从不复用。

### 对还原工具的意义

**这套规则只用于判读,不用于还原。** 三条硬约束:

1. 反向不唯一——多个原名共用一个新名(overload induction),从新名永远推不回原名;
2. `randomize=true` 时每次构建重新分配;
3. 实测存在未解释的偏差(见下),说明生成器还有未建模的内部状态。

因此还原链只认 mapfile,生成器规则只用来做**旁证**(判断某个 mapfile 是否与当前 jar 匹配)。

> **未解释偏差(如实记录)**:`alphabet="xy"` 的那次探针里,`Window` 的字段拿到序号 2/3/4(`xx, xy, yx`),而同一 jar 在 `alphabet="iJ"` 下是 0/1/2(`i, J, ii`)。两次探针的 `.dox` 只保留了后者,前者的配置无法复现,偏差原因不明。**结论:不要在任何实现里依赖"成员序号从 0 起"这个观察。**

## 自定义字母表:判读陷阱(通用)

`alphabet` 是纯配置值,任意字符集都能当字母表。由此得到两条**方向相反**的判读规则,必须同时成立才不误判:

- **正向不成立**:看到 i/I 单字母类名,不能推断"这是某个混淆器的硬编码特征"——它只是"该构建配过自定义字母表"的线索,任何支持 `alphabet` 的工具(含 DashO)都能一键产出。
- **反向成立**:凡单字母/重复字母/易混形类名(`i`/`I`、`l`/`1`、`O`/`0`),优先怀疑是**自定义字母表配置**而非默认行为;默认字母表不会产生这类名字。

### 为什么 `alphabet="iI"` 的类名是 `i, ii, iii, iiii…`

这个形态曾经被误读成"计数器把数字折叠进字母表"或"`minlength` 重复补齐",都不对。真实原因是**类名必须大小写不敏感唯一**(类名落盘成文件名,要能放进大小写不敏感的文件系统),生成器会把候选名与已分配的类名做**大小写折叠比较**,撞了就跳到下一个序号。

按双射 2 进制序列逐个试:

| 候选序号 | 候选名 | 折叠后 | 判定 |
|---|---|---|---|
| 0 | `i` | `i` | 空位 → 取 `i` |
| 1 | `I` | `i` | 撞 `i` → 跳 |
| 2 | `ii` | `ii` | 空位 → 取 `ii` |
| 3 | `iI` | `ii` | 撞 → 跳 |
| 4 | `Ii` | `ii` | 撞 → 跳 |
| 5 | `II` | `ii` | 撞 → 跳 |
| 6 | `iii` | `iii` | 空位 → 取 `iii` |
| … | | | 同款跳法,下一个空位是 `iiii` |

六个类名 `i, ii, iii, iiii, iiiiii` 全部对上,且**同一个 mapfile 里的成员名不受这条约束**——`ThreadsController` 的方法仍然是完整的 `i, I, ii, iI, Ii, II`(同名的 `i()` 与 `I()` 在同一个类里并存)。成员名不落盘成文件名,不需要大小写不敏感唯一。

这解释了两件事:

- `alphabet="iI"` 与 `alphabet="xy"` 生成的**分配序列完全相同**(只差字符映射),所以 `SnakeGame-renaming-iI.map` 与 `SnakeGame-renaming-default.map` 的成员新名逐条对应;
- `alphabet="iJ"` 下类名回到干净的 `i, J, ii, iJ, Ji, JJ`——`i`/`J` 折叠后仍是两个不同字母,没有碰撞可跳。

`minlength` 会让名字从双字母起步,`keeppublics` 会让 public 成员保持原名——"部分成员没被改名"不等于混淆器没跑。

## 保留规则(通用)

- **入口点标注只保类名,不保方法名**:入口类的类名保留,但其 `main([Ljava/lang/String;)V` 仍被改名。运行不受影响——Main-Class 指的是类名,`invokestatic` 走常量池重写。
- `<init>` / `<clinit>` 恒不改名。
- `serialVersionUID` 字段保留。
- **实现接口的方法保留**(`run()`、监听器回调等):接口方法名不能单方面改,与 Allatori 行为一致。
- `keeppackages=false`(默认)= **所有类平铺到默认包**,原包结构消失;`keeppackages=true` 时可用 class prefix 换包。

## mapfile(通用)

两种格式在**同一次运行**同时产出:

- **DashO 私有 `.map`**:`|OldClass|NewClass|` 段头 + 成员行 `=old=desc=new=`(方法)/ `+old+desc+new+`(字段),`desc` 为 JVM 描述符。
- **ProGuard 兼容 `.proguard.map`**(由 `proguardMap=` 属性指定):`old.name -> new:` + `retType oldName(args) -> newName` 行式。

两格式都含 `<init>`/`<clinit>` 的全等映射行,可用于自校验(解析器写对了这两行必然出现)。`mapoutput overwrite="true"` 必须,否则旧 map 阻断写入。

## 去混淆判据

- **有 mapfile**:查表精确还原,类/成员/注解全覆盖(含包路径)。本仓 `dasho.Rename` 即此路线,设计与拒绝面见 [rename-pass.md](rename-pass.md)。
- **无 mapfile**:只能靠 **entrypoint/接口保留名锚定 + 引用图差分**做近似恢复。单字母类名不含语义,不能从代码判原名;能恢复的只有"哪些名字是入口/接口(未改名)"这一层骨架。本仓**不实现**这一层——产出"看起来像原名"的合成名比保留 `a` 更有害。
- `renameAnnotations="on"` 时注解类型名也进 map。
- **mapfile 的类表里混着编造的名字**:`stringencrypt` 生成的解密器(`Owner$1`)与 `controlflow` 生成的假异常类(`Owner$XxxException`)也在表中,它们的"原名"是 DashO 按命名习惯编出来的,从未真实存在过。还原时若这些类还在,就会被"还原"成看似嵌套类的名字——因此 `dasho.ControlFlow` / `dasho.StringDecrypt` 必须排在 `dasho.Rename` 之前(见 [rename-pass.md](rename-pass.md) §6、§8)。
- **改名会放宽访问修饰符**:只开 renaming 的构建把包私有成员变成 `public`,mapfile 不记录该信息,还原后不可逆(见 [rename-pass.md](rename-pass.md) §7)。
- 未覆盖:`overload induction` 开启后的多签名共名形态与 map 表达(需单独取证)。
