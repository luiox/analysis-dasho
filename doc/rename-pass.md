# `dasho.Rename`:按 mapfile 还原改名的 pass 设计

本文是本仓 `dasho.Rename` 的实现设计。**通用技术形态**(配置面、生成名分配规则、mapfile 双格式)见 [renaming.md](renaming.md),本文只讲"还原这一步怎么做、为什么这样做"。

## 1. 设计立场:这是唯一一个不做推断的 pass

另外两个 pass 都是**语义级**还原——字符串加密、控制流注入都有可验证的结构判据,不认名字也能做。改名恰恰相反:它**销毁**了信息,而且销毁得不可逆。

`renaming.md` §生成名的分配规则已经证明:新名来自"序号 → 字母表"的确定性映射,但反推不成立——overload induction 让多个原名共用一个新名,`randomize=true` 让同一份代码每次构建名字都不同。所以本 pass 的输入不是判据,而是**外部证据**:构建时产出的 mapfile。

由此定下三条:

1. **没有 mapfile 就不干活。** `dasho.Rename` 只有在 pack 配置给了 `map` 时才注册——注册一个必然空转的 pass 只会误导 `--list`。
2. **不做"像原名"的猜测。** 不用 `a`→`Class1` 这类合成名。宁可留着 `a`,也不产出假语义。
3. **mapfile 与 jar 不配套时宁可不动。** 见 §5。

## 2. 反演:mapfile 是 old→new,计划要 new→old

mapfile 记的是 `原名 → 混淆名`,而还原要把混淆名改回原名,所以每条规则的首尾两段都要对调:

```
mapfile:  |DataOfSquare|a|   =lightMeUp=(I)V=a=   +color+I+b+
计划:      a → DataOfSquare   a.a(I)V → lightMeUp   a.b:I → color
```

这一层做成纯函数(`src/dasho/restore_plan.cpp`),不碰容器、不碰 IO,单测直接喂文本断言计划内容。

## 3. 描述符预映像:错了不报错,只是静默不命中

这是最容易写错、且错了**没有任何症状**的地方。

mapfile 里的描述符在**原名空间**(`getValAleaNotInSnake()LTuple;`),而混淆后的类文件里同一方法是 `()Ll;`。重映射计划按 `(owner, name, desc)` 三元组精确匹配,于是:

| 键 | owner | name | desc |
|---|---|---|---|
| 源键(必须) | `j`(混淆名) | `iJ`(混淆名) | `()Ll;`(混淆空间) |
| 目标键(必须) | `ThreadsController` | `getValAleaNotInSnake` | `()LTuple;`(原名空间) |

源键写错 → 规则永不命中 → pass 跑完一个字节没改,日志还显示"装载了 N 条规则"。所以:

- 先装**全部类映射**(new→old),再装成员规则——成员规则的 owner 要靠类映射预映像;
- 描述符用 `jbc::type::classNameSpans` 逐段改写,语法非法时**显式失败**并计数,不静默返回原串。

单测 `RestorePlan.PreimagesDescriptorInObfuscatedSpace` 用"原名空间描述符必须查不到"做反证,防止这条断言变成碰巧成立。

## 4. 改写面:必须全开

`applyResourceRemapPlan(container, plan, RemapPolicy::all())` 一次覆盖:

| 面 | 内容 | 漏了会怎样 |
|---|---|---|
| bytecode | this/super/interfaces、成员声明与引用、指令操作数、异常表 `catch_type`、SMT 的 Object 校验类型、InnerClasses/Nest 族 | `NoSuchMethodError` / `VerifyError` |
| reflectionStrings | `Class.forName("a")`、`getMethod("x")` 这类常量 | `ClassNotFoundException` |
| annotations | 注解元素值 | 运行期读注解拿到旧名 |
| signatures | Signature 属性、LVTT 行 | 泛型解析指回旧名(不炸,但反编译看到 `a`) |
| resources | `a.class` → `原名.class`(含包路径重建)、`META-INF/services` provider 行、MANIFEST 的 `Main-Class` 族 | 类文件在、`java -jar` 起不来 |

**半改比不改更糟**:类改名后只要漏一面就留下悬垂引用,jar 能导出但运行期炸。所以本 pass 不做"只改类名/只改成员"的分档——要么全开,要么不做。

帧不需要再生:改名不改栈形,`StackMapTable` 只需重指向校验类型;CP 只追加不重编号,布局稳定。

## 5. 三道拒绝门

| 门 | 判据 | 后果 |
|---|---|---|
| 无 mapfile | pack 配置没有 `map` 项 | pass 不注册 |
| 计划为空 | mapfile 解析出 0 条可还原规则 | `PreservedAnalyses::all()`,打 warn |
| URI 冲突 | 目标 `原名.class` 已被占,且占用者自己不会被改走 | 该**类**整类放弃(含其成员规则),其余照常还原 |

第三道门防的是"mapfile 与 jar 不配套"(拿错构建的 map、jar 已被别的工具改过)。容器是 `unordered_map`,两条类规则抢同一个 URI 时后写覆盖先写——**静默丢类**。判定在 URI 层做,不解析任何类字节:候选集合 = 容器内全部资源 URI,腾挪空间 = 全部会被改走的混淆名。

判定为冲突后只跳过**那一条类规则**,不是整个 pass——一个 jar 里大部分类仍然能正确还原。

## 6. mapfile 里有一部分名字是编造的

实测(三重混淆样本)mapfile 的类表同时含三类条目:

| 条目 | 例子 | 来源 |
|---|---|---|
| 真类 | `DataOfSquare→a`、`Window→n` | 应用自己的类 |
| 字符串解密器 | `DataOfSquare$1→b` | `stringencrypt` 生成的匿名类 |
| 假异常类 | `ThreadsController$ParseException→k`、`Main$Exception→g` | `controlflow tryCatch` 生成 |

后两类的"原名"是 DashO **编造**的:它按 `<Owner>$<数字>` / `<Owner>$<Xxx>Exception` 的命名习惯给生成类起了一个看起来合理的原名,好让 mapfile 自洽。**这些类从来没有以那个名字存在过。**

对还原工具的影响:

- 假异常类由 `dasho.ControlFlow` 先行删除,轮到 `dasho.Rename` 时那些类规则自然不命中——顺序对了就没有副作用;
- **单独跑 `dasho.Rename` 会把假异常类"还原"成 `Main$Exception` 这类看似嵌套类的名字**,凭空造出结构。这是"必须按 `ControlFlow → StringDecrypt → Rename` 排序"的又一个理由(见 §8);
- 字符串解密器类同样会被还原成 `Owner$1`。`dasho.StringDecrypt` 现在会清扫死解密器(见 [string-decrypt-pass.md](string-decrypt-pass.md) §7),所以这条也只在单跑 `Rename` 时出现。

## 7. 改名本身还会动访问修饰符

实测:`SnakeGame` 只开 renaming 构建后,原本包私有(`package-private`)的类与成员**全部变成 `public`**;同一 jar 只开 stringencrypt 的构建则不动访问标志。mapfile 不记录访问标志,因此**这一步不可逆**——还原后这些成员仍是 `public`。

本 pass 不尝试猜回原访问级别(没有依据)。这是已知残留差异,与 `nop` 填充、`SourceFile` 残留并列(见 §9)。

## 8. 顺序:ControlFlow → StringDecrypt → Rename

三重混淆样本(`stringencrypt` + `controlflow tryCatch` + `renaming`)的实测结论:

- **`ControlFlow` 必须在 `StringDecrypt` 之前。** DashO 的执行序是 stringencrypt → controlflow,所以控制流注入**打在了解密器自己身上**。解密器一旦被塞进 try-catch 和块切分,就不再"纯且可解释",求值式判据直接失效(实测:先解字符串 → 折叠 0 个站点;先摘控制流 → 折叠 3 个)。
- **`Rename` 放最后。** 它只认名字,前两个 pass 都不依赖名字,先做哪个都不影响;但放最后能吃到前两个 pass 删掉的生成类,减少无意义规则,日志也更干净。

## 9. 已知残留差异

| 差异 | 原因 | 能否还原 |
|---|---|---|
| 成员访问标志被放宽为 `public` | renaming 构建的副作用,mapfile 不记录 | 否 |
| `SourceFile` 仍是混淆名(`a.java`) | mapfile 不记录;虽然常等于"新类名 + .java",但那是惯例不是契约 | 否(不猜) |
| 个别方法多一条 `nop` | DashO 改写时的填充 | 否(语义等价,无害) |
| 泛型签名里的类型变量名 | 不属改名面 | 否 |

## 10. 测试与回归

单测(`test/unittest/restore_plan_test.cpp`,9 例)钉住四个易错点:方向、描述符预映像(含反证)、同名条目跳过、URI 冲突判定;另测描述符改写的边界(数组、原始类型、非法输入)。
