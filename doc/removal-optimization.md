# DashO 裁剪/优化/Make-Synthetic/方法调用移除技术档案

本文描述 DashO 12.8 `removal` / `optimization` / `make-synthetic` / `methodCallRemoval` 四个功能的**通用技术形态**:配置语义、官方文档要点、字节码层的可观察副作用,以及这些副作用对去混淆判据的影响。

## Removal(裁剪)

- 分析未用代码,删除未用类/方法/字段;可选"全部"或"仅非 public"。
- 反射访问的类/成员可能误判为未用——建议 entrypoint 或 exclude 标注;"Determine Reflected Classes" 尝试简单反射检测(仅类级,方法/字段不可)。
- Debug 属性移除:SourceFile、SourceDebugExtension、LineNumberTable、LocalVariableTable、LocalVariableTypeTable、Signature、Deprecated、Synthetic、InnerClasses、注解可见性(RuntimeVisible/Invisible + Parameter + TypeAnnotation)——可配置保留哪些(如保留行号供生产栈迹)。

## Optimization(优化)

- 代数恒等、强度削减、peephole 优化,加速 + 减体积。
- make-public 策略:为保重命名后访问权限,把类/非私有成员改 public;默认"看似必要时才改",可强制;禁用可能导致访问异常。
- Android Mode 走 R8 做同类优化。

## Make Synthetic

- 把 private/package-private(或含 protected)类/方法/字段标 synthetic,干扰反编译器。
- 库混淆时建议 "Never"/"Only private and package"/"If not public","Always" 会破坏库 API 暴露。
- **去混淆侧:识别解密器时不能依赖 synthetic 标志**(与 Allatori synthetic 泛滥同思路)。

## Method Call Removal(方法调用移除)

- 移除指定方法的调用点(方法本身不删);典型用途:生产环境去掉日志调用。
- 仅直接指定类上的调用被移除(子类继承调用不移,因 JVM 运行时决议);最有效对象是 static/final 类。
- 不支持 regex;不能 exclude;非 void 调用移除后返回默认值(0/false/null),可能产生副作用(`double ignored = call()` 会 unbox null)。

## 入口点配置语义(removal 专属,通用)

裸 `<classes name="X"/>` **不会 root 类内方法**。官方文档原文:"If you specify a class without any methods or fields … only the class itself and methods that override other retained methods will be considered entry points"。

后果:若只写裸类名,removal 会把整个应用判为 unused——`main()` 被删、只留下入口类与几个内部类,产物远小于输入。**manifest 里的 `Main-Class` 不能兜底**:一旦定义了 `<entrypoints>`,自动检测就被跳过。正确写法:

```xml
<entrypoints>
  <classes name="X">
    <method name="main" signature="java.lang.String[]"/>
  </classes>
</entrypoints>
```

`signature` 是 Java 风格全限定(`java.lang.String[]`),**不是** JVM 描述符。

## 字节码层的可观察副作用(通用)

### Removal

- `classes="unused" members="unused"`:零引用的类/成员被**真删**(不是只改标志)。小应用在"入口点 + 活集覆盖"下通常一个类都不删,但**全 jar 零引用的成员(如未被调用的 getter)会被删**——这是判断 removal 是否生效的正面证据。
- 默认 debug 属性清单(SourceFile、SourceDebugExtension、LineNumberTable、LocalVariableTable、LocalVariableTypeTable)被剥 → **栈迹变成 `at X.<init>(Unknown Source)`**。行号剥离是**用户可见副作用**,也是文档"可保留行号供生产栈迹"选项的存在理由。
- `Signature` 不在默认剥离清单,保留。
- 若输入本身由 javac 未产 LocalVariableTable,LVT 剥离就无对照可验(取证时需先确认输入有该属性)。

### Optimization

- **devirtualization**:private 方法补 `ACC_FINAL`(`0x0002` → `0x0012`)。这是 optimization 最稳的可观测信号。
- **peephole 常量去重**:相邻同值常量压栈合并为 `dup`(`sipush X; sipush X` → `sipush X; dup`),代码区缩小。
- **SE 调用点的 `nop` 标记消失**:未优化产物在每个字符串解密调用点后带 `nop`,开 optimization 后消失 → 说明 **optimization 实际执行在 stringencrypt 之后**,与项目文件里元素的书写顺序无关。这是判定 pass 真实执行序的一个可观测信号,值得记入任何 pass 顺序假设。
- **不折叠 SE 的外层 key 算术链**:`dup2/swap/iadd` 与简式 `iadd` 并存且原样保留 → **常量折叠必须由去混淆侧自己做**,不能指望 optimization 帮忙(判据见 [string-encryption.md](string-encryption.md) §3.3)。

### Make-Synthetic

- 类/字段加 `ACC_SYNTHETIC`;方法加 `ACC_SYNTHETIC | ACC_BRIDGE`(含 static 方法)——反编译器诱饵。
- **entrypoint 不豁免**:入口类与 `main()` 同样被标 synthetic + bridge。对照 renaming/removal 对 entrypoint 的保护,synthetic 不管这套;JVM/launcher 也不在乎(synthetic+bridge 的 static main 照常可执行)。
- **对解密器识别的干扰**:解密器类与它的全部方法同样被污染(类 0x0021 → 0x1021,方法 0x0009 → 0x1049)→ 确证 [string-encryption.md](string-encryption.md) 的结论:**识别必须按方法体形状**,flags / synthetic / 名字都不可用。

### 生成类指纹(独立于 synthetic)

DashO 自己生成的 helper / 解密器类:

| 属性 | 值 |
|---|---|
| classfile version | **45.3**(`major=45` / `minor=3`) |
| `attributes` | **0**(无 SourceFile、无 LineNumberTable) |

用户类通常 `major` 52–61 且带 SourceFile/LineNumberTable。**"45.3 + 零属性"是"这个类不是 javac 产出的"的稳定锚点**,可用于圈定注入/生成的类集合——且与是否开 synthetic 无关,比 flags 可靠。
