# deobf 代码组织约定

本文件是**本仓代码组织的规范**，同时作为跨仓（`analysis-dasho` / `deobf` / 未来的 `zkm-deobf`）的共同约定。

它不规定"怎么判据、怎么改写"——那是各混淆器家族自己的知识。它只规定**代码放在哪、叫什么、按什么顺序写、怎么测**。

## 0. 一句话

> **deobf 代码的组织单位是「一种残留（residue）」，每种残留 = 一份会变的家族知识 + 三段恒定的动作（scan / rewrite / proof）；跨残留复用只允许发生在三段各自的工具箱里；对接 pipeline 只是一张数据表。**

---

## 1. 为什么是这个抽象

### 1.1 本质

```
混淆器：  B' = T_n ∘ … ∘ T_1 (B)        有序、带参数、含随机化
去混淆：  D  = T_k⁻¹ ∘ … ∘ T_1⁻¹        每个 T_i⁻¹ 都是【偏函数】
```

三个推论：

1. **只能逐个变换求逆** → 代码的原子单位是"一种残留的逆"，不是"一个工具"。
2. **T_i⁻¹ 是偏函数** → 第一件事永远是找证据，不是改代码。
3. **顺序由混淆器决定** → 顺序是**外部知识**，不属于 deobf 核心（见 §9）。

### 1.2 三个轴，变化率完全不同

| 轴 | 变化率 | 例子 |
|---|---|---|
| **动作**（scan / rewrite / proof） | **恒定** | 任何混淆器的残留都要识别、改动、保证 |
| **对象**（residue 种类） | 随**家族**变 | ZKM 有 long-key fold，DashO 没有 |
| **知识**（family facts） | 随**家族 × 版本**变 | runtime 类名、模板形状、Era、mapfile 格式 |

**组织方式 = 让"变的"有固定落点，"不变的"不许被污染。** 纵向扩展多版本的全部答案就是：加版本只允许动第三轴。

### 1.3 为什么不用 `pass`

| 候选 | 判定 | 原因 |
|---|---|---|
| `pass` | ❌ | 来自编译器的**遍历**模型，描述"怎么跑"，不描述"在处理什么" |
| `transform` | ⚠️ 半 | 是**混淆器侧**视角；且变换常是复合的（`dasho.Checks` 覆盖 6 种检查） |
| **`residue`（残留）** | ✅ | 是 **artifact 侧**视角——deobf 代码唯一能看见的就是它 |
| `evidence` | ✅ | 残留的超集，把 mapfile 这类旁证也纳进来，`Rename` 才有位置 |

`pass` 这个词**允许出现在 `adapter/` 里**（它是流水线的编排词汇），**禁止出现在 `src/` 里**。

### 1.4 三段是**关注点分解**，不是阶段分解

| | 阶段分解（stage） | **关注点分解（concern）** |
|---|---|---|
| 含义 | 必须按序执行 | 对同一个对象做的三类**不同性质**的工作 |
| 需要顺序吗 | 需要 | **不需要** |
| 需要多态吗 | 需要 | **不需要**（就是三组自由函数） |

去掉顺序承诺之后，三段可以是三个自由函数、三个目录、三个编译单元——**关注点分解不产生调用关系约束**。这正是它比"pass 链"强的地方：pass 链强迫你把顺序写进代码结构，而顺序其实是外部知识。

---

## 2. 仓库形状（横向：每个混淆器家族仓完全一致）

```
<family>-deobf/
├── README.md                        # 身份 + 残留一览表(必含【判据】列)
│                                    # + 全局不变量 + 语料回归命令
├── CONVENTION.md                    # 本文件
├── doc/
│   ├── README.md                    # 索引 + 口径分工声明
│   ├── <residue>.md                 # 残留的技术形态(artifact 侧，换输入仍成立才写)
│   └── <residue>.impl.md            # 三章固定：Scan / Rewrite / Proof
│
├── src/<family>/
│   ├── knowledge/                   # ← 第三轴：唯一允许随版本变的地方
│   │   ├── era.*                    #    Era 枚举 + 探测规则
│   │   ├── runtime_names.*          #    runtime 类名 / descriptor 常量表
│   │   └── templates.*              #    模板形状常量(S3 级判据的数据)
│   │
│   ├── <residue>.hpp / .cpp         # ← 第一+二轴：一种残留一个文件
│   │                                #    三段 = 三个自由函数 + 文件内固定分区
│   │
│   ├── scan_toolbox.*               # ← 复用只允许发生在这三处
│   ├── rewrite_toolbox.*            #    见 §7
│   └── proof_toolbox.*              #
│
│   ├── <pipeline>_adapter.*         # ← 整目录可替换，见 §9
│   └── container.*                  # ← 家族无关的画布(装载/索引/写回)
│
└── test/
    ├── unittest/<residue>/scan_test.cpp     # 喂形状 → 断言 ScanResult 与拒绝原因
    ├── unittest/<residue>/rewrite_test.cpp  # 喂 ScanResult → 断言字节码
    └── corpus-regress.sh                    # 端到端
```

---

## 3. 三段契约

### 3.1 签名（头文件，自由函数）

```cpp
namespace dasho::<residue> {

/// SCAN：纯读。唯一有跨站点可变状态的段。
ScanResult scan(const ScanContext&, const mjbase::model::Container&);

/// REWRITE：纯写。状态是按类/按站点的局部，不进 ctx。
DirtySet rewrite(const Knowledge&, mjbase::model::Container&, const ScanResult&);

/// PROOF：纯判。几乎无状态，判定 + 回滚。
ProofResult proof(const Knowledge&, const mjbase::model::Container&,
                  const DirtySet&, TraceSink&);

}
```

### 3.2 五条禁止句（可 grep 检查）

```
1. src/<residue>.cpp 内不许出现 pass / pipeline / stage 词汇
   （`DashoXxxPass::run` 这类适配层入口与 `dom.order()` 这类库 API 不在此列，
     但适配层迁到 adapter/ 之后应当清零）
2. knowledge/ 之外不许出现任何混淆器版本相关的字面量
3. scan    不许修改任何字节码 / 异常表 / 常量池        （纯读）
4. rewrite 不许回头找站点，只能消费 ScanResult          （禁止重新扫描）
5. proof   不许修改，只判定 + 回滚                       （禁止"修一下让它过"）
```

**可执行检查**：`pwsh -File tools/check-convention.ps1`。
三档结果 FAIL（真违规）/ TODO（约定已定、代码未迁移）/ OK；退出码只看 FAIL。
规则 3/5 靠"分区标记内出现改写调用"来近似——文件一旦打了 `==== SCAN / REWRITE /
PROOF ====` 分区，语义约束就退化成文本约束。

**第 4 条最有价值**，它带来四个直接后果：

1. 版本分支只可能落在 scan → §6 的 Era 规则从"纪律"变成"结构必然"
2. 可分别测试：scan 单测喂指令形状，rewrite 单测喂 ScanResult，互不污染
3. 可审计：ScanResult 落盘 = 站点级证据链
4. 可组合：一个残留的 ScanResult 能被另一个残留消费

### 3.3 文件内分区

`<residue>.cpp` 按固定顺序分区，**不强制拆成三个文件**：

```cpp
// ==== SCAN ====================================================================
//   粗找：...（可钉形状，便宜、高召回）
//   细找：...（必须钉事实）        判据等级：S2 结构事实
//   拒绝：...（fail-closed；这里是 bug 的聚集地）
//   输出：ScanResult{ sites[], rejects[] }
// ==== REWRITE =================================================================
//   改写策略的选择条件：...
// ==== PROOF ===================================================================
//   类级门：帧再生 + 校验器 → 失败回滚该类
//   家族特有验收：...
// =============================================================================
```

**拆文件条件**（写死，避免过早/过晚拆）：某段 > 300 行，或被 ≥ 2 个残留引用 → 才提为 `*_toolbox.*`。

---

## 4. `ScanResult` 数据契约

三段之间**只有一条数据流**：

```cpp
struct Site {
    Location at;      // 类 / 方法 / 指令区间 / 异常表项
    Evidence why;     // 支撑判定的东西（可空 = 旁证驱动）
    Action   do_;     // Remove / Replace / Rename / Collapse / Keep
    Grade    grade;   // S1 / S2 / S3，见 §6
    Reject*  reject;  // 非空 = 拒绝，带原因（供 trace 与文档）
};

struct ScanResult {
    std::vector<Site> sites;
    std::vector<Site> rejects;
};
```

**这是数据形状，不是接口。** 各残留的 `ScanResult` 内容天差地别——

| 残留 | `Site.why` 里装什么 |
|---|---|
| `string_encryption` | 求值出的明文常量 |
| `control_flow_injection` | 可达集 + 摘空/保布局决策 |
| `runtime_checks` | 区间边界 + handler 分组 |
| `dead_filler` | 空转窗口范围 + 活跃性结果 |
| `renaming_map` | 重映射规则（反演后的 new→old） |

——但**形状相同**，所以 adapter 能用同一段代码处理它们，而不需要 `IStringDecryptStrategy` 这类按功能分的接口。加新混淆器时你加的是新的 `why` 内容，不是新的接口实现。

---

## 5. ctx 按**可变性**分层（否则它会退化成另一个 `this`）

> **`this` 就是隐式 ctx，ctx 就是显式 `this`。** 两者的差别不是能力，是依赖可见性。

deobf 里真正需要的状态只有三样，**生命周期完全不同**：

| 类型 | 生命周期 | 可变性 |
|---|---|---|
| `Knowledge`（Era、家族常量、模板、runtime 名） | 跨 run | **只读** |
| `AnalysisCache`（纯度、CFG、可达集） | 恰好一个 run | **可变**，仅 scan 写 |
| `TraceSink`（拒绝原因、站点日志） | 每次调用 | **只写** |

```cpp
struct Knowledge     { Era era; /* 家族常量/模板/runtime 名 */ };
struct AnalysisCache { /* 纯度、CFG、可达集 */ };
struct TraceSink     { void reject(SiteId, Grade, const char* reason); };

struct ScanContext {
    const Knowledge& k;
    AnalysisCache&   cache;   // ← 只有 scan 有
    TraceSink&       trace;
};
```

**三种可变性 = 三个类型 = 测试时可单独构造、单独断言。** 塞进一个 `this` 就把三种可变性压成一种，分不清"这是配置还是缓存"，测试就得先 reset 对象。

### 5.1 三段的状态需求**本来就不对称**——别为了对称硬造三个 ctx

| 段 | 状态需求 | 为什么 |
|---|---|---|
| **scan** | **重**：跨站点的昂贵分析缓存 | 全包遍历、纯度递归、CFG/可达集复用 |
| **rewrite** | **局部**：按类/按站点的编辑器会话、dirty 表 | 改写是局部动作，站点之间不共享状态 |
| **proof** | **几乎无**：判定 + 回滚，每次独立 | 帧再生、校验器都是按类一次性调用 |

**签名的不对称本身就是文档**：谁读了都会知道"只有识别这一步需要缓存"。强行给三段都套一个 `XxxContext`，等于用形式对称掩盖实质不对称——这和"每个都叫 pass"是同一类错误。

### 5.2 虚函数只允许出现在两处

1. **adapter**——而且正确形态是**数据表，不是类**（见 §9）
2. **细找内部的 Era 策略**——仅当 `switch (era)` 分支爆炸到不可维护时；**只活在细找内部，不许外溢成三段级别的接口**

**除此之外禁止虚函数。** 一旦出现 `virtual ScanResult scan()`，`DashoStringDecryptPass` 那个类就会复活——它同时是编排单元和三段宿主，`pass` 这个词就跟着回来了。

### 5.3 例外：`.cpp` 内自由，`.hpp` 受约束

`rewrite` 内部若有一组私有辅助函数共享局部状态（编辑器会话、跳转重定向表），**允许在匿名 namespace 里造小 struct**：

```cpp
namespace {
struct RegionEditor {   // 不进头文件，不可继承，不算"类"
    jbc::ir::CodeUnit& cu;
    ca::usize erase(...);
};
}
```

判据：**出现在 `.hpp` 里就是接口，出现在 `.cpp` 里就是局部变量的组织方式。** 约定只约束头文件——因为只有头文件会被别人 include、被 adapter 看见、被下一代当成可继承的东西。

---

## 6. 判据等级与 Era

### 6.1 粗找 / 细找的分工律

| | 优化目标 | 可以钉什么 |
|---|---|---|
| **粗找** | 召回率 + 便宜 | **可以钉形状**（反正细找会确认） |
| **细找** | 精度 + 不变量 | **必须钉事实**（混淆器无法在保持功能的前提下改掉） |

**粗找钉形状是合法的**——它只需要"不漏"，不需要"不错"。这一条让粗找可以随版本快速改，细找才需要稳定。

### 6.2 细找的强度分级

细找钉的"事实"分两种：

- **语义后果**：混淆器**为了功能必须如此**，改了自己就坏了
  → `catch_type` 指向的类必须从不被实例化；调用点区间必须能算出常量
- **实现选择**：混淆器**碰巧如此**，下个版本就能换
  → 反射三连模板、匿名 `$1` 类命名、无字段

| 等级 | 含义 | 跨版本稳定性 |
|---|---|---|
| **S1** | 语义后果 | 几乎不用改 |
| **S2** | 结构事实 | 跨构建稳定，跨版本需复核 |
| **S3** | 实现选择 | **版本敏感，Era 分支唯一合法落点** |

**证据就在本仓 README 里**：`dasho.Checks` 的判据**改了三轮**（钉的是 S3 实现选择），`dasho.ControlFlow` **一次成型**（钉的是 S1 语义后果）。

### 6.3 Era 规则

```cpp
enum class Era : uint8_t { V12, V13, Unknown };   // 值，不是类型
Era probeEra(const mjbase::model::Container&);     // 每个家族仓固定提供这一个函数
```

- ❌ 不按版本分目录（`v12/` `v13/` → 版本爆炸，且猜不到下一个）
- ❌ 不为每个版本实现 `IxxxCriteria`（回到接口陷阱）
- ✅ **混淆 jar 由单一混淆器版本产出 → 版本可可靠探测 → 探测结果是一个值**

**硬规则**：版本分支只允许出现在 **scan 的细找**里，且必须是 `S3` 级判据；**禁止出现在 rewrite 和 proof**。

理由来自本质：改写策略（结构 vs 保布局）取决于"摘完帧还能不能重算"这种**语义事实**，与版本无关；判据才取决于版本。违反这条 = 版本逻辑渗进改写 = 下个版本两处都要改。

`Era::Unknown` → **fail-closed 拒绝，不做猜测性改写**。

---

## 7. 工具箱 = 复用的唯一合法位置

**跨残留的复用只允许发生在三个工具箱里，不许在 `<residue>.cpp` 之间直接互相调用。**

| 工具箱 | 内容 | 现状 |
|---|---|---|
| `scan_toolbox` | 求值器、CFG 后继图、区间/活跃性、纯度判定、锚点 | `jvm_interp` ✅ `cfg.hpp` 的图部分 ✅ `ClassSet::isPure` ✅ |
| `rewrite_toolbox` | 区域删除、goto 线程化、不可达清理、跳到下一条清理、溢写对折叠 | `pruneUnreachable` `tidyGotoToNext` `foldStoreLoadPairs` `threadGotos` ✅ 已收口进 `cfg.hpp` |
| `proof_toolbox` | 校验器、类数检查、悬垂检查、逐类加载、帧再生 | `ClassSet::commit`（帧链）✅ `test/tools/VerifyJar.java` ✅ |

### 7.3 当前落地形态

三个工具箱在本仓**还没有各自的 `*_toolbox.*` 文件**，而是折叠在
`cfg.hpp` 内的两个分区（`==== SCAN 工具箱 ====` / `==== REWRITE 工具箱 ====`）
加上 `class_set.hpp`（容器层的 proof）。

按 §3.3 的拆文件条件（> 300 行，或被 ≥ 2 个残留引用），`cfg.hpp` 已经 545 行、
被 4 个残留引用，两条都踩了——**它是下一个该拆的文件**。拆之前先保持现状：
工具箱的**归属**必须清楚，物理上是否单独成文件是次要的。

### 7.1 检查方法（可执行）

> **数三个工具箱各有几份实现，>1 份 = 复用没收口。**

反例（`ZKM-27-Full-Deobfuscator`，44000 行 / 0 测试）：17 份 `verifyArchive`、19 份 `sha256`、11 份 `writeVerifiedArchive`、5 份 `previousCode`——**proof 工具箱没收口**，于是每个残留文件都自带一整套三段。

### 7.2 本仓曾有的问题（本分支已修）

`src/dasho/cfg.hpp` 的头注释曾声称"这里的函数全部是**只读**的"，但文件里同时装着
scan 工具箱（`buildGraph` `reachSet` `computeAlive`）和 rewrite 工具箱
（`pruneUnreachable` `tidyGotoToNext` `foldStoreLoadPairs`）——**注释在撒谎，两个
工具箱混在一个文件**，而且 `threadGotos` 还单独留在 `cleanup_pass.cpp` 里当私货，
被 `control_flow` 那边用不上。

本分支已按 §3.3 重新分区：`cfg.hpp` 现在是 `基础（指令身份/标签/流形）→
==== SCAN 工具箱 ==== → ==== REWRITE 工具箱 ====` 三段，`threadGotos` 收口进
REWRITE 段，头注释改成如实描述。`check-convention.ps1` 会对"同一原语出现两份实现"
直接判 FAIL。

---

## 8. 文档与测试口径（跨仓统一）

### 8.1 文档

```
doc/<residue>.md          只写"混淆器会产出什么结构" —— 换个输入 jar 仍成立才写
doc/<residue>.impl.md     只写"本仓这个残留怎么还原" —— 三章固定 Scan / Rewrite / Proof
仓外笔记                   具体样本取证(哪个 jar、哪条指令、round-trip 值) —— 不进仓
README 残留一览表           必须有【判据】列 + 等级(S1/S2/S3) —— 这是全仓的"本质声明"
```

### 8.2 测试

| 层 | 测什么 | 形状 |
|---|---|---|
| **判据测试** | scan 的细找 + 拒绝面 | 喂构造的指令形状 → 断言**接受/拒绝**与拒绝原因 |
| **改写测试** | rewrite | 喂 `ScanResult` → 断言输出的指令序列/异常表形态 |
| **语料回归** | 端到端 | 退出码 + **类数不增** + 逐类加载校验 + golden |

**"类数不增 + 逐类加载校验"是跨仓通用验收口径**——不依赖任何混淆器知识，却能抓住大部分改写错误（尤其是帧错误：改写出错往往落在没被执行到的分支上）。

---

## 9. adapter 边界

deobf 核心**零 pipeline 知识**。适配器就是一张数据表：

```cpp
// <pipeline>_adapter.cpp   ← 整个文件可以替换，核心不认识它
struct ReversalEntry {
    const char*  name;                                    // "dasho.StringDecrypt"
    ScanResult (*scan)(const ScanContext&, const Container&);
    DirtySet   (*rewrite)(const Knowledge&, Container&, const ScanResult&);
    ProofResult(*proof)(const Knowledge&, const Container&, const DirtySet&, TraceSink&);
    const char** before;      // 顺序 = 适配器的元数据
    const char** after;
};
```

| 归 adapter（可整目录替换） | 归 deobf 核心（`src/`） |
|---|---|
| 名字与注册 | 三段 + `ScanResult` 词汇表 |
| **顺序声明**（`before`/`after`） | 家族知识（`knowledge/`） |
| **Era 探测**（调一次，结果注入三段） | 三个工具箱 |
| 状态上报（成功/失败/跳过/拒绝计数） | 容器（装载/索引/写回） |
| 迭代与收敛（fixed-point） | |
| trace 输出通道 | |
| 发布门（整包不发布） | |

**换 pipeline = 换 adapter 目录，`src/` 一行不动。**
反过来：`src/` 里出现 pipeline 词汇 = 越界，§3.2 第 1 条直接判违规。

---

## 10. 本分支的迁移状态

### 已做

- [x] 本文件（组织约定成文）
- [x] `cfg.hpp` 按 SCAN / REWRITE 两个工具箱重新分区，修正"全部只读"的失实注释
- [x] `threadGotos` 从 `cleanup_pass.cpp` 收口进 `cfg.hpp` 的 REWRITE 工具箱
- [x] 5 个残留文件补 `==== SCAN / REWRITE / PROOF ====` 分区标记 + 判据等级标注
- [x] README 残留一览表加【等级】列（S1/S2/S3）
- [x] `doc/README.md` 声明 `*-pass.md` 的三章固定结构
- [x] `tools/check-convention.ps1` 把 §3.2 五条禁止句变成可执行检查
      （当前 **FAIL 0 / TODO 9**）

### 待做（就是 `check-convention.ps1` 打印的那份清单）

| # | 规则 | 处数 | 具体 |
|---|---|---|---|
| 1 | §3.2-1 词汇 | 3 | 适配层入口 `DashoXxxPass::run` 与 `registerXxx` 仍在残留文件里；另有 3 处注释里写着 "pass" |
| 2 | §3.2-2 版本常量 | 1 | `control_flow_pass.cpp:54` 的 `kGeneratedVersion`（45.3）该进 `knowledge/` |
| 3 | §3.2-4 rewrite 回头找站点 | 4 | `bypassNeutralRuns` 调 `computeLiveness`+`neutralWindow`；`control_flow` step4 调 `computeReachable`；`rewriteInPlace` 调 `anchorDepthUniform` |
| 4 | §3.2-5 proof 修改了 | 1 | `control_flow` step6 在 PROOF 段删死类 |

第 3 组是**真正的架构债**：这 4 处的 scan 与 rewrite 还没被 `ScanResult` 切开，
识别和改写挤在同一个函数里。迁完之后把 `check-convention.ps1` 里这两条的 sev 从
`TODO` 提成 `FAIL`，规则才算真的生效。

其余按优先级：

- [ ] `ScanResult` 类型落地，让 5 个残留真正产出它（当前各残留仍是各自的局部容器）
- [ ] `knowledge/` 落地：`45.3`、反射三连、`RuntimeException` 这些家族常量抽成常量表
- [ ] `Era` 枚举 + `probeEra()`（当前只有 12.8 一族；落点已定在 `control_flow` 的生成类指纹与 `checks` 的模板）
- [ ] adapter 目录落地，`pack.cpp` 迁入 —— 迁完后 #1 的 3 处词汇 TODO 清零
- [ ] `doc/<residue>.impl.md` 三章化（现有 `doc/*-pass.md` 是两份混写的形态/实现文档）
- [ ] 单测按 `scan_test` / `rewrite_test` 重新分组
