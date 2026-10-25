# DashO 运行时检查框架(Checks)技术档案

本文描述 DashO 12.8 运行时检查框架(checks)的**通用技术形态**:配置面、运行时库、注入产物结构、检查调用点模板,以及识别与摘除判据。具体样本的取证细节(哪个 jar、注入了哪些方法、还原出哪些常量)见仓外研究目录。

## 配置面(dasho.xsd ChecksType)

- `debuggingCheck` / `debugEnabledCheck`:调试状态检查。
- `emulatorCheck`:模拟器检查。
- `hookCheck`:hook 框架检查(Frida/Xposed 类)。
- `rootCheck`:root 检查。
- `shelfLifeCheck`:有效期检查(配 expiry 日期/token)。
- `tamperCheck`:篡改检查(签名/完整性)。
- 各 `*Response`:触发后响应,可带 `probability`(概率触发)。
- `response` 取值:`none` / `hang` / `exit` / `exception` / `error`;`where` 取值:`Beginning` / `End` / `BeginningAndEnd`。

### 哪些 check 在 Java SE 上可用(实测)

`<runtime target="java">` 下 DashO **直接拒绝** Android 专属的三种,报 `"<X>Check can only be used on Android."` 并以 exit=1 中止构建:

| check | Java SE | 备注 |
|---|---|---|
| `debuggingCheck` | ✅ | |
| `debugEnabledCheck` | ✅ | |
| `emulatorCheck` | ❌ Android only | 构建期直接报错 |
| `hookCheck` | ❌ Android only | 同上 |
| `rootCheck` | ❌ Android only | 同上 |
| `shelfLifeCheck` | ⚠️ | 要 `tokengenerator` 产出的 token/key 文件,缺了报 "Key file/token source not specified" |
| `tamperCheck` | ⚠️ | 要签名别名,缺了报 "Alias for signing key is not set" |

这条把"6 种 check"的对抗面**砍到 4 种**,其中 2 种还要额外资产。剩下 4 种共用同一套注入模板(同一个 `Bootstrap` 门面、同一套 helper),所以剥离判据不必按 check 类型分叉。

### 配置陷阱(通用)

- **`where="End"` 会改变方法的形状**:DashO 把原表达式的值**溢写到局部变量**,注入段接在后面,检查跑完再读回来。一行 getter 因此从 `getfield; ireturn` 变成 `getfield; istore_1; <注入段>; iload_1; ireturn`。剥离时要一并收掉这个溢写对(见 [checks-strip-pass.md](checks-strip-pass.md) §6.1)。
- **`where` 可能被静默忽略**:某些方法形态(例如只有 `while(true)` 循环、没有正常出口的 `run()`)拿不到 `End` 注入点,DashO 不报错、只是不注入。所以"配了没生效"要按方法逐个查 `Class.forName` 锚点,不能按配置推断。
- **一次注入可能有 2–3 条 `Class.forName`**:实测同一处注入里出现 2 条或 3 条反射链。推断与 `Bootstrap` 的分层状态机有关(`LAYER_TRAMPOLINE` / `LAYER_THREAD` / `LAYER_MAPS` / `LAYER_CRC` + `poll()`),未逐层验证。所以"一个方法一个锚点"是错的假设,判据要按"区间内至少一条"。
- **`locations` 必须带 `method` 子元素,`signature` 用 Java 风格**:全限定类型、`[]` 后缀、逗号分隔多参(`java.lang.String[]`),**不是** JVM 描述符(`[Ljava/lang/String;`)。填描述符会得到 "checks were unused during injection" 且 exit=1;空参方法填 `signature=""`。
- **只写 `<classes name="X"/>` 不带 `method` 时整个 check 判 unused**——不会静默成功,而是直接报错。
- **必须配 `<global><option>force</option></global>`**(等价命令行 `--force`):注入码自带 `Class.forName`/`getMethod` 反射,会撞上 "Reflection Halts Build" 设置冲突。建议直接写进项目文件。
- `<report path="..."/>` 产出文本报告(入口点/反射使用/统计),是定位注入位置最直接的手段——"Reflection use <类>.<方法> - java.lang.Class.forName()" 这一行就是注入点清单。报告里的 In/Out 类计数点在**注入之后、renaming/removal 之前**——不能拿来当输入类数。

## 运行时库

- `lib/shelflife-java.jar` / `shelflife-android.jar`:有效期检查实现。
- `lib/bootstrap-runtime.jar`:运行时引导。
- `lib/DecryptLib.jar` 的 `dasho/a..m` 属资源加密解密流,**与 checks 无关**(见 [decryptlib.md](decryptlib.md));checks 运行时仅靠 shelflife/bootstrap-runtime。

## 官方文档要点

- 注入式检查(debug/emulator/hook/root/shelf life/tamper)+ 分离式响应:
  - 立即退出(随机非零返回码)、空栈异常(隐藏来源)、回调应用方法、概率触发(probability)、延迟 Response。
  - Check 与 Response 分离增加攻击难度;可多次混用不同配置。
  - 自定义动作先于响应执行;两者都不选则跳过并告警。
- shelf life:token 生成(tokengenerator CLI)、到期/警告日期。
- tamper:完整性校验(签名相关)。

## 注入产物形态(通用)

开 `<injection option="on">` + 任一 `*Check` 后,DashO 会向输出追加两类结构。

### 1. 运行时门面类

`com/preemptive/common/Bootstrap`(+ 一个内部类)是检查的运行期门面:

- 常量串 `LAYER_TRAMPOLINE` / `LAYER_THREAD` / `LAYER_MAPS` / `LAYER_CRC` / `OVERALL_COMPROMISED` / `FAIL_CLOSED`——分层状态机;
- `native` 方法 `init()` / `poll()` / `setLogLevel(int)`。

检查主体在 native 侧,Java 层只是**反射门面**。这直接决定了注入点的形态:必然出现 `Class.forName` + `Class.getMethod` + `Method.invoke` 三连,也正是构建期 force 必配的原因。

### 2. 每个被注入类的伪内部类 `{Owner}$N`

每被注入一个 owner,就在其旁生成 `{Owner}$1`、`{Owner}$2`…,内含"伪装成 String API 名"的静态 helper。**结构恒定**:无字段、无 `<init>`、全部 `static`、恰好两个方法。

| 形态 | 作用 |
|---|---|
| `{Name}(String,int,int)String` 等,char 循环 XOR + 位混洗 | 字符串解密器(与 stringencrypt 同款,见 [string-encryption.md](string-encryption.md)) |
| `{Name}()I` 返回 `Random.nextInt(75)+1` | **噪声种子发生器**:每次调用变化,是不透明谓词的取值源 |
| 名字取 `replace` / `copyValueOf` / `concat` / `regionMatches` / `startsWith` / `endsWith` / `indexOf` / `insert` / `subSequence` / `valueOf` / `onsite` 等 | 通用串处理 helper,无实际语义 |

"无字段 + 无 `<init>` + 全 static" 这一条是判据里最有用的:**它把 helper 壳与任何真实工具类区分开**。一个类如果既没有实例状态又没有构造器,它只可能被静态调用——而 DashO 正是这么设计的(helper 只在注入段里出现)。

同一 helper 会被**解密与检查两条链路复用**——按类名或方法名归类都不可靠,只能按方法体形状归类。

## 检查调用点模板(通用骨架)

```
; 0) 噪声分支:()I 种子使 (r*N)%r ≡ 0 恒真 → if<c> 的选路是确定的,不是随机
invokestatic {Owner}$N.{seed}:()I
dup; iconst_<k>; imul; swap; irem; if<c> L_alt
ldc <密串A>
bipush <a>; bipush <b>; invokestatic {Owner}$N.{dec}:(String,II)String
goto L_join
L_alt:
ldc <密串B>
bipush <a>; bipush <b>; invokestatic {Owner}$N.{dec}:(String,II)String
L_join:
nop
; 1) 反射调 Bootstrap 门面(方法名同样走密串 + 算术包装)
invokestatic java/lang/Class.forName:(String)Class
invokevirtual java/lang/Class.getMethod:(String,[Class])Method
invokevirtual java/lang/reflect/Method.invoke:(Object,[Object])Object   ; → 检查结果 List
; 2) 遍历 List,startsWith/substring 解析分层状态
; 3) 命中 → invokestatic java/lang/System.exit:(I)V     ; response=exit
; 4) 放行后才进入原逻辑
```

要点:

1. **双支 ldc 解密后同值**——这是真混淆(两支等价),不是"真支/诱饵"结构。与 stringencrypt 的双支诱饵形态相反,不能套用同一个还原假设。
2. **整段被 `try { ... } catch (Exception) { pop }` 包住**:吞掉一切反射异常,失败即静默放行(fail-open)。异常表覆盖整个注入段。
3. **`exit` 响应的返回码是编译期常量**,不是运行期随机——官方文档说的"random non-zero"发生在构建期取值,同一构建内固定。
4. **注入码自带轻量控制流混淆**:无效果算术(`bipush a; bipush b; imul; bipush c`、`swap; imul; idiv`)散布在调用点之间,与 controlflow 同族(见 [control-flow.md](control-flow.md))。

## 识别与摘除判据

检查注入**没有专属标识**——用的全是 JDK 类,`Class.forName` / `System.exit` 在正常代码里到处都是。所以判据必须是两层的:**模板形状** + **区间结构**,两者同时成立才判。

### 第一层:模板形状(三条,位置无关)

| 判据 | 内容 | 注入器绕不开的原因 |
|---|---|---|
| 反射三连 | `Class.forName` + `Class.getMethod` + `Method.invoke` 同现于区间内 | 检查主体在 native 侧,Java 层只能反射 |
| 噪声谓词 | `invokestatic ()I` → `dup` → 整型常量 → `imul` → `swap` → `irem` → 条件跳转,七条连排 | 不透明谓词模板本身,`(seed*k)%seed ≡ 0`;换档只换常量与跳转方向 |
| 响应 | `System.exit` / `Runtime.halt` / `Thread.sleep` / `athrow` | 没有响应就没有检查,DashO 自己会判 unused |

前两条与第三条都只在区间内**任意位置**匹配,不要求顺序或相邻——块切分/块乱序会打散相邻性。唯一的相邻判据是噪声谓词的七条窗口,它安全的原因是反向的:真实业务代码不会平白写一条 `x = (f()*4) % f()` 再拿结果分支。

### 第二层:区间结构(hammock)

注入段必然被 `try { … } catch (Exception) { … }` 整段包住(fail-open),因此:

- **起点** = 包含 `forName` 锚点的异常表项的 `from`。但要按 **handler 分组**:块切分会把一条 `try` 拆成几十段表项而共用同一 handler,只取单段会把起点算到方法中段。
- **右端** = 跳转落点集合中,能构成 hammock 且**覆盖方法内全部 `forName` 锚点**的最小值。少了"覆盖全部锚点"这条,块切分产生的后半段会留在原地。
- **hammock 三条件**:区间外没有边进入区间**内部** `(f, X)`、区间内后继要么在内要么恰好是 `X`、区间内每个位置要么从 `f` 可达要么**全局不可达**(容忍 DashO 撒的死填充)。
  注意第一条是 `(f, X)` 而非 `[f, X)`:**外部边进入 `f` 必须允许**——`where="End"` 时注入段前面就是原逻辑,必然有一条落下边。拒绝它会让所有 `End` 注入剥不掉。唯一要拒的是回边(源在 `X` 之后却跳回 `f`)。
- **异常边按全可能加**(不判 `canThrow`):多算只会让判据更严,少算才会误删。

### 摘除

```
1. 先摘异常表项（整条落在区间内的）
2. 再销毁只被摘除项引用的边界标签（Boundary 角色会保护锚点）
3. 最后删指令（删完标签句柄失效，表项就匹配不上了）
4. 异常表清空后清死填充 + 清 goto→next
5. 折叠 `where="End"` 留下的 xSTORE n / xLOAD n 溢写对
6. 三路候选 ∪ 活引用兜底，删残留 helper / Bootstrap
```

顺序不可换:先删指令会留下陈旧表项(校验器直接拒);先摘表项就 `eraseBatch` 会吃 `StillUsed`(err=2)。

### 几个易错点

- **`Bootstrap` 类名不可当锚点**:renaming 会改掉它。稳定的是形态(≥1 `native` + 全 static + 无实例字段)与 `LAYER_*` 常量串。
- **helper 锚定按形状**:`()I` 无参静态方法 + `Random.nextInt` → 噪声种子;char 循环 XOR → 解密器。不要按方法名(`replace`/`concat` 这类名字本身就是伪装)。
- **helper 会被预生成**:DashO 为入口闭包里的每个类都生成一份 `{Owner}$N`,很多从未被调用。所以死类候选不能只取"注入段引用过的",还要按形态补。
- **别和 tryCatch 混判**:那里 `catch` 只吞不可达的 `athrow` 且 `catch_type` 是生成的假异常类;这里 `catch` 真的吞反射异常且类型是 `java/lang/Exception`。

实现细节见 [checks-strip-pass.md](checks-strip-pass.md)。
