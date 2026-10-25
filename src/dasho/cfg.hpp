/// @file        cfg.hpp
/// @brief       各 pass 共用的字节码控制流底座：指令身份键、标签解析、流形状分类。
///
/// 三个 pass（StringDecrypt / ControlFlow / Checks）都要回答同一个问题："这条
/// 指令会把控制交给谁"。答案的唯一可靠来源是 libjbc 的 `CodeDom` + 标签解析，
/// 不能从 `Insn` 的原始操作数读——分支偏移在 IR 里已经变成标签句柄，偏移只在
/// lower 之后才重新成立（JVMS 6.5：偏移相对于分支指令自身的 pc）。
///
/// 这里的函数全部是**只读**的：不改指令、不改异常表。改写由各 pass 自己用
/// `CodeEditor` 做，因为改写策略（拒绝 / 改锚 / 保布局）是每个 pass 自己的语义。
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

namespace dasho {

/// 指令句柄 → 可哈希的键。`InsnId` 是"所有者作用域内"的句柄（slot + 代数），
/// 同一条指令在不同 CodeUnit 里 slot 可能相同，所以只能在单方法内当键用。
inline ca::u64 keyOf(jbc::ir::InsnId id) noexcept
{
    return (static_cast<ca::u64>(id.slot) << 32) | static_cast<ca::u64>(id.gen);
}

/// 方法的顺序布局：order[i] 是第 i 条指令，pos 是反向索引。
struct Layout
{
    std::vector<jbc::ir::InsnId>           order;
    std::unordered_map<ca::u64, ca::usize> pos;
};

inline Layout layoutOf(const jbc::ir::CodeDom& dom)
{
    Layout lay;
    lay.order = dom.order();
    lay.pos.reserve(lay.order.size() * 2);
    for (ca::usize i = 0; i < lay.order.size(); ++i) {
        lay.pos.emplace(keyOf(lay.order[i]), i);
    }
    return lay;
}

/// 标签 → order 下标；方法末尾标签解析为 size()（== 区间右开边界）。
///
/// 解析失败返回 false——调用方必须当成"这个方法不碰"，不能拿 0 顶替：把
/// "解析不出来"当成"指向第 0 条"会让可达性算错，进而删掉活指令。
inline bool indexOfLabel(const jbc::ir::CodeDom& dom,
                         jbc::ir::LabelId      label,
                         const Layout&         lay,
                         ca::usize&            out)
{
    auto resolved = dom.resolveLabel(label);
    if (resolved.is_err()) return false;
    const jbc::ir::LabelTarget target = std::move(resolved).unwrap();
    if (target.isEnd()) {
        out = lay.order.size();
        return true;
    }
    const auto it = lay.pos.find(keyOf(target.insn));
    if (it == lay.pos.end()) return false;
    out = it->second;
    return true;
}

/// 控制流形状分类。`Subroutine` 单独出来：jsr/ret 是 Java 6 以前的子程序
/// 形态，可达性要按"返回地址集合"算，本仓的 pass 都不做，遇到即放弃该方法。
enum class Flow : ca::u8 { Fall, Jump, Cond, Switch, Stop, Subroutine };

inline Flow flowOf(jbc::core::Op op) noexcept
{
    switch (op) {
    case jbc::core::Op::GOTO:
    case jbc::core::Op::GOTO_W:
        return Flow::Jump;
    case jbc::core::Op::IFEQ:
    case jbc::core::Op::IFNE:
    case jbc::core::Op::IFLT:
    case jbc::core::Op::IFGE:
    case jbc::core::Op::IFGT:
    case jbc::core::Op::IFLE:
    case jbc::core::Op::IF_ICMPEQ:
    case jbc::core::Op::IF_ICMPNE:
    case jbc::core::Op::IF_ICMPLT:
    case jbc::core::Op::IF_ICMPGE:
    case jbc::core::Op::IF_ICMPGT:
    case jbc::core::Op::IF_ICMPLE:
    case jbc::core::Op::IF_ACMPEQ:
    case jbc::core::Op::IF_ACMPNE:
    case jbc::core::Op::IFNULL:
    case jbc::core::Op::IFNONNULL:
        return Flow::Cond;
    case jbc::core::Op::TABLESWITCH:
    case jbc::core::Op::LOOKUPSWITCH:
        return Flow::Switch;
    case jbc::core::Op::IRETURN:
    case jbc::core::Op::LRETURN:
    case jbc::core::Op::FRETURN:
    case jbc::core::Op::DRETURN:
    case jbc::core::Op::ARETURN:
    case jbc::core::Op::RETURN:
    case jbc::core::Op::ATHROW:
    case jbc::core::Op::RET:
        return Flow::Stop;
    case jbc::core::Op::JSR:
    case jbc::core::Op::JSR_W:
        return Flow::Subroutine;
    default:
        return Flow::Fall;
    }
}

// ── 后继图 ──

/// 后继表。多留一格给"方法末尾"伪节点（下标 == 指令数），这样方法末尾标签
/// （`indexOfLabel` 解析成 size()）不需要特殊分支。
struct Graph
{
    std::vector<std::vector<ca::usize>> succ;
};

/// 建后继表。返回 false 表示"这个方法不碰"——标签解析失败或出现 jsr/ret。
inline bool buildGraph(const jbc::ir::CodeUnit& cu, Graph& g)
{
    const auto&     order = cu.dom.order();
    const ca::usize n     = order.size();
    const Layout    lay   = layoutOf(cu.dom);

    g.succ.assign(n + 1, {});
    for (ca::usize i = 0; i < n; ++i) {
        const jbc::ir::Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) return false;

        std::vector<ca::usize>& out = g.succ[i];
        switch (flowOf(in->op)) {
        case Flow::Subroutine:
            return false;
        case Flow::Stop:
            break;
        case Flow::Fall:
            if (i + 1 < n) out.push_back(i + 1);
            break;
        case Flow::Cond: {
            ca::usize t = 0;
            if (!indexOfLabel(cu.dom, in->target, lay, t)) return false;
            out.push_back(t);
            if (i + 1 < n) out.push_back(i + 1);
            break;
        }
        case Flow::Jump: {
            ca::usize t = 0;
            if (!indexOfLabel(cu.dom, in->target, lay, t)) return false;
            out.push_back(t);
            break;
        }
        case Flow::Switch: {
            ca::usize t = 0;
            if (!indexOfLabel(cu.dom, in->target, lay, t)) return false;
            out.push_back(t);
            for (const jbc::ir::LabelId l : in->targets) {
                if (!indexOfLabel(cu.dom, l, lay, t)) return false;
                out.push_back(t);
            }
            break;
        }
        }
    }

    // 异常边：try 区间内每条指令都可能把控制交给 handler。这里按"全可能"
    // 加边（不判 canThrow）——区间判定要的是**保守的入边集合**：多算边只会
    // 让区间判据更难成立（更不容易误删），少算才会删掉活代码。
    for (const jbc::ir::CodeUnit::Exc& e : cu.exc) {
        ca::usize s = 0, t = 0, h = 0;
        if (!indexOfLabel(cu.dom, e.start, lay, s)) return false;
        if (!indexOfLabel(cu.dom, e.end, lay, t)) return false;
        if (!indexOfLabel(cu.dom, e.handler, lay, h)) return false;
        for (ca::usize i = s; i < t && i < n; ++i) {
            g.succ[i].push_back(h);
        }
    }
    return true;
}

/// 从 f 出发、不经过 X 的可达集合。
inline void reachSet(const Graph& g, ca::usize f, ca::usize X, std::vector<bool>& seen)
{
    seen.assign(g.succ.size(), false);
    std::vector<ca::usize> work{f};
    seen[f] = true;
    while (!work.empty()) {
        const ca::usize i = work.back();
        work.pop_back();
        for (const ca::usize v : g.succ[i]) {
            if (v == X || seen[v]) continue;
            seen[v] = true;
            work.push_back(v);
        }
    }
}

/// 全方法可达集：入口 + 每个 handler 入口。
///
/// 用来识别"本来就不可达"的填充块。DashO 的控制流注入会在方法体里撒
/// `goto L; nop; nop; athrow; L:` 这种永不执行的填充，它们落在注入段内部但
/// 不从任何地方可达——删掉没有语义影响，所以区内容忍这一类。
inline void computeAlive(const Graph&                   g,
                         const std::vector<ca::usize>&  handlers,
                         std::vector<bool>&             alive)
{
    const ca::usize n = g.succ.size() - 1;
    alive.assign(g.succ.size(), false);
    std::vector<ca::usize> work;
    const auto mark = [&](ca::usize i) {
        if (i >= n || alive[i]) return;
        alive[i] = true;
        work.push_back(i);
    };
    mark(0);
    for (const ca::usize h : handlers) mark(h);

    while (!work.empty()) {
        const ca::usize i = work.back();
        work.pop_back();
        for (const ca::usize v : g.succ[i]) mark(v);
    }
}

/// 删掉方法里**全局不可达**的指令。
///
/// 控制流注入会在方法体里撒 `goto L; nop; nop; nop; athrow; L:` 这种永不执行
/// 的填充块。它们不执行，但帧再生只给"可达的分支目标"发帧，死块又需要帧才能
/// 过校验（classfile ≥ 50），于是产物会 `VerifyError: Expecting a stack map
/// frame`。把死块删干净，帧就自然对上了。
///
/// 只在异常表已空时做：还有真 handler 时，handler 体可能是"只被异常边可达"，
/// 按普通可达性算会误删（那条边要不要算取决于 verifier 的保守合并，见
/// doc/control-flow-strip-pass.md §3.2）。
inline ca::usize pruneUnreachable(jbc::ir::CodeUnit& cu)
{
    if (!cu.exc.empty()) return 0;

    Graph g;
    if (!buildGraph(cu, g)) return 0;

    std::vector<bool> alive;
    computeAlive(g, {}, alive);

    const auto&                 order = cu.dom.order();
    std::vector<jbc::ir::InsnId> dead;
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (!alive[i]) dead.push_back(order[i]);
    }
    if (dead.empty()) return 0;

    jbc::ir::CodeEditor editor(cu);
    if (editor.eraseBatch(dead, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
            .is_err()) {
        return 0;
    }
    return dead.size();
}

/// 清掉"跳到紧邻下一条"的 goto。
///
/// 注入段在方法头部时，真代码前面必然留一条 `goto L1`；把填充段与 handler 删掉
/// 之后，L1 就落在 goto 的下一条，goto 变成纯粹的噪声（反编译器会为它造一个
/// 单块循环或直接抱怨）。删它对语义零影响，却让输出立刻可读。
///
/// 用 `kRetargetToFallthrough`：若还有别的分支跳这条 goto，先把它们改锚到下一条
/// 再删；异常表边界引用按 StillUsed 拒绝——本函数只在异常表已清空的方法上调用。
inline ca::usize tidyGotoToNext(jbc::ir::CodeUnit& cu)
{
    ca::usize removed = 0;
    for (;;) {
        const auto&     order = cu.dom.order();
        const ca::usize n     = order.size();
        if (n < 2) return removed;

        const Layout lay = layoutOf(cu.dom);
        jbc::ir::InsnId victim = jbc::ir::kNoInsn;
        for (ca::usize i = 0; i + 1 < n; ++i) {
            const jbc::ir::Insn* in = cu.dom.peek(order[i]);
            if (in == nullptr) continue;
            if (in->op != jbc::core::Op::GOTO && in->op != jbc::core::Op::GOTO_W) continue;
            ca::usize t = 0;
            if (!indexOfLabel(cu.dom, in->target, lay, t)) continue;
            if (t != i + 1) continue;
            victim = order[i];
            break;
        }
        if (victim == jbc::ir::kNoInsn) return removed;

        jbc::ir::CodeEditor editor(cu);
        if (editor.erase(victim, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                .is_err())
        {
            return removed;
        }
        ++removed;
    }
}

// ── 局部变量指令分类 ──

/// 局部变量指令的槽位下标（`_0.._3` 缩略形态与显式索引）。
inline ca::i32 localSlotOf(const jbc::ir::Insn& in)
{
    switch (in.op) {
    case jbc::core::Op::ILOAD_0: case jbc::core::Op::LLOAD_0: case jbc::core::Op::FLOAD_0: case jbc::core::Op::DLOAD_0: case jbc::core::Op::ALOAD_0:
    case jbc::core::Op::ISTORE_0: case jbc::core::Op::LSTORE_0: case jbc::core::Op::FSTORE_0: case jbc::core::Op::DSTORE_0: case jbc::core::Op::ASTORE_0:
        return 0;
    case jbc::core::Op::ILOAD_1: case jbc::core::Op::LLOAD_1: case jbc::core::Op::FLOAD_1: case jbc::core::Op::DLOAD_1: case jbc::core::Op::ALOAD_1:
    case jbc::core::Op::ISTORE_1: case jbc::core::Op::LSTORE_1: case jbc::core::Op::FSTORE_1: case jbc::core::Op::DSTORE_1: case jbc::core::Op::ASTORE_1:
        return 1;
    case jbc::core::Op::ILOAD_2: case jbc::core::Op::LLOAD_2: case jbc::core::Op::FLOAD_2: case jbc::core::Op::DLOAD_2: case jbc::core::Op::ALOAD_2:
    case jbc::core::Op::ISTORE_2: case jbc::core::Op::LSTORE_2: case jbc::core::Op::FSTORE_2: case jbc::core::Op::DSTORE_2: case jbc::core::Op::ASTORE_2:
        return 2;
    case jbc::core::Op::ILOAD_3: case jbc::core::Op::LLOAD_3: case jbc::core::Op::FLOAD_3: case jbc::core::Op::DLOAD_3: case jbc::core::Op::ALOAD_3:
    case jbc::core::Op::ISTORE_3: case jbc::core::Op::LSTORE_3: case jbc::core::Op::FSTORE_3: case jbc::core::Op::DSTORE_3: case jbc::core::Op::ASTORE_3:
        return 3;
    default:
        return in.a;
    }
}

/// 局部变量指令的**类型族**：不同族之间不构成一对。
enum class LocalKind : ca::u8 { Int, Long, Float, Double, Ref, None };

inline LocalKind localKindOf(const jbc::core::Op op)
{
    switch (op) {
    case jbc::core::Op::ILOAD: case jbc::core::Op::ILOAD_0: case jbc::core::Op::ILOAD_1: case jbc::core::Op::ILOAD_2: case jbc::core::Op::ILOAD_3:
    case jbc::core::Op::ISTORE: case jbc::core::Op::ISTORE_0: case jbc::core::Op::ISTORE_1: case jbc::core::Op::ISTORE_2: case jbc::core::Op::ISTORE_3:
        return LocalKind::Int;
    case jbc::core::Op::LLOAD: case jbc::core::Op::LLOAD_0: case jbc::core::Op::LLOAD_1: case jbc::core::Op::LLOAD_2: case jbc::core::Op::LLOAD_3:
    case jbc::core::Op::LSTORE: case jbc::core::Op::LSTORE_0: case jbc::core::Op::LSTORE_1: case jbc::core::Op::LSTORE_2: case jbc::core::Op::LSTORE_3:
        return LocalKind::Long;
    case jbc::core::Op::FLOAD: case jbc::core::Op::FLOAD_0: case jbc::core::Op::FLOAD_1: case jbc::core::Op::FLOAD_2: case jbc::core::Op::FLOAD_3:
    case jbc::core::Op::FSTORE: case jbc::core::Op::FSTORE_0: case jbc::core::Op::FSTORE_1: case jbc::core::Op::FSTORE_2: case jbc::core::Op::FSTORE_3:
        return LocalKind::Float;
    case jbc::core::Op::DLOAD: case jbc::core::Op::DLOAD_0: case jbc::core::Op::DLOAD_1: case jbc::core::Op::DLOAD_2: case jbc::core::Op::DLOAD_3:
    case jbc::core::Op::DSTORE: case jbc::core::Op::DSTORE_0: case jbc::core::Op::DSTORE_1: case jbc::core::Op::DSTORE_2: case jbc::core::Op::DSTORE_3:
        return LocalKind::Double;
    case jbc::core::Op::ALOAD: case jbc::core::Op::ALOAD_0: case jbc::core::Op::ALOAD_1: case jbc::core::Op::ALOAD_2: case jbc::core::Op::ALOAD_3:
    case jbc::core::Op::ASTORE: case jbc::core::Op::ASTORE_0: case jbc::core::Op::ASTORE_1: case jbc::core::Op::ASTORE_2: case jbc::core::Op::ASTORE_3:
        return LocalKind::Ref;
    default:
        return LocalKind::None;
    }
}

inline bool isLocalLoad(const jbc::core::Op op)
{
    switch (op) {
    case jbc::core::Op::ILOAD: case jbc::core::Op::ILOAD_0: case jbc::core::Op::ILOAD_1: case jbc::core::Op::ILOAD_2: case jbc::core::Op::ILOAD_3:
    case jbc::core::Op::LLOAD: case jbc::core::Op::LLOAD_0: case jbc::core::Op::LLOAD_1: case jbc::core::Op::LLOAD_2: case jbc::core::Op::LLOAD_3:
    case jbc::core::Op::FLOAD: case jbc::core::Op::FLOAD_0: case jbc::core::Op::FLOAD_1: case jbc::core::Op::FLOAD_2: case jbc::core::Op::FLOAD_3:
    case jbc::core::Op::DLOAD: case jbc::core::Op::DLOAD_0: case jbc::core::Op::DLOAD_1: case jbc::core::Op::DLOAD_2: case jbc::core::Op::DLOAD_3:
    case jbc::core::Op::ALOAD: case jbc::core::Op::ALOAD_0: case jbc::core::Op::ALOAD_1: case jbc::core::Op::ALOAD_2: case jbc::core::Op::ALOAD_3:
        return true;
    default:
        return false;
    }
}

/// 折叠 `xSTORE n; xLOAD n` 相邻对。
///
/// `where="End"` 的注入会把原表达式的值先溢写到局部变量，等检查跑完再读回来
/// （`getX()` 那种一行方法因此变成 `getfield; istore_1; <整段注入>; iload_1;
/// ireturn`）。注入段删掉之后，这一对只剩"写进去、立刻读回来"，净栈效应为 0，
/// 删掉它语义不变，反编译结果会干净很多。
///
/// 四条安全条件：
///   1. 两条**在 `order` 里直接相邻**；
///   2. **load 不是分支目标**——`order` 只含指令，两条之间夹一个 label 时它们
///      照样相邻。若有分支直接跳到那条 load，`kRetargetToFallthrough` 会把该边
///      改锚到对之后，那条路径就丢了这一对本来"净效应 0"的存取（栈形不变，
///      但原本期望从局部变量取值的地方拿不到值）——所以必须显式排除；
///   3. 同族同槽（`istore_1` 配 `iload_1`，不配 `aload_1`）；
///   4. 方法里**没有别的指令读这个槽**（含 `iinc`）——那才是"这个值只给这条
///      load 用"。实例方法的 `this`（槽 0）通常被读很多次，自然被挡住。
inline ca::usize foldStoreLoadPairs(jbc::ir::CodeUnit& cu)
{
    const auto&                  order = cu.dom.order();
    std::vector<jbc::ir::InsnId> victims;

    // 分支/switch 目标的位置集合（判据 2）。解析失败就当"整方法不碰"。
    const Layout          lay = layoutOf(cu.dom);
    std::vector<bool>     isTarget(order.size(), false);
    for (ca::usize k = 0; k < order.size(); ++k) {
        const jbc::ir::Insn* in = cu.dom.peek(order[k]);
        if (in == nullptr) return 0;
        const Flow f = flowOf(in->op);
        if (f != Flow::Jump && f != Flow::Cond && f != Flow::Switch) continue;
        ca::usize t = 0;
        if (!indexOfLabel(cu.dom, in->target, lay, t)) return 0;
        if (t < order.size()) isTarget[t] = true;
        for (const jbc::ir::LabelId l : in->targets) {
            if (!indexOfLabel(cu.dom, l, lay, t)) return 0;
            if (t < order.size()) isTarget[t] = true;
        }
    }

    for (ca::usize i = 0; i + 1 < order.size();) {
        const jbc::ir::Insn* a = cu.dom.peek(order[i]);
        const jbc::ir::Insn* b = cu.dom.peek(order[i + 1]);
        if (a == nullptr || b == nullptr) {
            ++i;
            continue;
        }
        const LocalKind ka = localKindOf(a->op);
        if (ka == LocalKind::None || !isLocalLoad(b->op) || localKindOf(b->op) != ka) {
            ++i;
            continue;
        }
        const ca::i32 slot = localSlotOf(*a);
        if (slot != localSlotOf(*b)) {
            ++i;
            continue;
        }
        if (isTarget[i + 1]) {   // 有人直接跳到那条 load
            ++i;
            continue;
        }

        bool otherRead = false;
        for (ca::usize k = 0; k < order.size() && !otherRead; ++k) {
            if (k == i + 1) continue;
            const jbc::ir::Insn* in = cu.dom.peek(order[k]);
            if (in == nullptr) continue;
            if (isLocalLoad(in->op) && localSlotOf(*in) == slot) otherRead = true;
            if (in->op == jbc::core::Op::IINC && static_cast<ca::i32>(in->a) == slot) otherRead = true;
        }
        if (otherRead) {
            ++i;
            continue;
        }

        victims.push_back(order[i]);
        victims.push_back(order[i + 1]);
        i += 2;
    }

    if (victims.empty()) return 0;

    jbc::ir::CodeEditor editor(cu);
    if (editor.eraseBatch(victims, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
            .is_err()) {
        return 0;   // 有边界标签还在引用：放弃，不动
    }
    return victims.size();
}

}  // namespace dasho
