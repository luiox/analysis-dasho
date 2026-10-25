#include "dasho/cleanup_pass.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "dasho/cfg.hpp"
#include "dasho/class_set.hpp"

namespace dasho {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using jbc::core::Op;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

namespace {

/// 轮数上限。三个步骤互相解锁（删死块让 goto 相接、旁路让新死块出现），
/// 实测两轮即收敛；给到 4 轮纯属兜底，超限也照常返回——结果仍然自洽，
/// 只是可能留着下一轮才清的残渣。
constexpr ca::usize kMaxRounds = 4;

// ── 栈效应白名单 ──

struct Delta
{
    int pop  = 0;
    int push = 0;
};

/// 一条指令的栈效应。
///
/// 只收录**确定不抛、无副作用、效应固定**的指令。返回 false 就整块放弃：
/// 判据的方向是"只要有一丝不确定就不动"，因为旁路的正确性完全依赖
/// "这段代码对程序状态没有任何贡献"。
///
/// 刻意不收的几类：
///   - `idiv`/`irem`（`ArithmeticException`）、数组读写与 `getfield`（NPE）、
///     `checkcast`（`ClassCastException`）——会抛就不是无贡献；
///   - `invoke*`、`putfield`、`monitor*`——有可观测副作用；
///   - `ldc` 的 Class/MethodType/MethodHandle/Dynamic 常量——首次解析可能
///     触发类加载，删掉会改变可观测行为。
bool deltaOf(jbc::ir::ClassSession& session, const Insn& in, Delta& d)
{
    switch (in.op) {
    case Op::NOP:
        return true;

    case Op::ACONST_NULL:
        d.push = 1;
        return true;

    case Op::ICONST_M1:
    case Op::ICONST_0:
    case Op::ICONST_1:
    case Op::ICONST_2:
    case Op::ICONST_3:
    case Op::ICONST_4:
    case Op::ICONST_5:
    case Op::BIPUSH:
    case Op::SIPUSH:
    case Op::FCONST_0:
    case Op::FCONST_1:
    case Op::FCONST_2:
        d.push = 1;
        return true;

    case Op::LCONST_0:
    case Op::LCONST_1:
    case Op::DCONST_0:
    case Op::DCONST_1:
        d.push = 2;
        return true;

    case Op::LDC:
    case Op::LDC_W:
    case Op::LDC2_W: {
        jbc::ir::ClassSession::ConstSem c;
        if (!session.constOf(in, c)) return false;
        switch (c.kind) {
        case jbc::ir::ClassSession::ConstSem::Kind::Int:
        case jbc::ir::ClassSession::ConstSem::Kind::Float:
        case jbc::ir::ClassSession::ConstSem::Kind::String:
            d.push = 1;
            return true;
        case jbc::ir::ClassSession::ConstSem::Kind::Long:
        case jbc::ir::ClassSession::ConstSem::Kind::Double:
            d.push = 2;
            return true;
        default:
            return false;
        }
    }

    case Op::ILOAD:
    case Op::ILOAD_0:
    case Op::ILOAD_1:
    case Op::ILOAD_2:
    case Op::ILOAD_3:
    case Op::FLOAD:
    case Op::FLOAD_0:
    case Op::FLOAD_1:
    case Op::FLOAD_2:
    case Op::FLOAD_3:
    case Op::ALOAD:
    case Op::ALOAD_0:
    case Op::ALOAD_1:
    case Op::ALOAD_2:
    case Op::ALOAD_3:
        d.push = 1;
        return true;

    case Op::LLOAD:
    case Op::LLOAD_0:
    case Op::LLOAD_1:
    case Op::LLOAD_2:
    case Op::LLOAD_3:
    case Op::DLOAD:
    case Op::DLOAD_0:
    case Op::DLOAD_1:
    case Op::DLOAD_2:
    case Op::DLOAD_3:
        d.push = 2;
        return true;

    case Op::ISTORE:
    case Op::ISTORE_0:
    case Op::ISTORE_1:
    case Op::ISTORE_2:
    case Op::ISTORE_3:
    case Op::FSTORE:
    case Op::FSTORE_0:
    case Op::FSTORE_1:
    case Op::FSTORE_2:
    case Op::FSTORE_3:
    case Op::ASTORE:
    case Op::ASTORE_0:
    case Op::ASTORE_1:
    case Op::ASTORE_2:
    case Op::ASTORE_3:
        d.pop = 1;
        return true;

    case Op::LSTORE:
    case Op::LSTORE_0:
    case Op::LSTORE_1:
    case Op::LSTORE_2:
    case Op::LSTORE_3:
    case Op::DSTORE:
    case Op::DSTORE_0:
    case Op::DSTORE_1:
    case Op::DSTORE_2:
    case Op::DSTORE_3:
        d.pop = 2;
        return true;

    case Op::IINC:
        return true;

    case Op::POP:
        d.pop = 1;
        return true;

    case Op::POP2:
        d.pop = 2;
        return true;

    case Op::DUP:
        d.pop  = 1;
        d.push = 2;
        return true;

    case Op::DUP_X1:
        d.pop  = 2;
        d.push = 3;
        return true;

    case Op::DUP_X2:
        d.pop  = 3;
        d.push = 4;
        return true;

    case Op::DUP2:
        d.pop  = 2;
        d.push = 4;
        return true;

    case Op::DUP2_X1:
        d.pop  = 3;
        d.push = 5;
        return true;

    case Op::DUP2_X2:
        d.pop  = 4;
        d.push = 6;
        return true;

    case Op::SWAP:
        d.pop  = 2;
        d.push = 2;
        return true;

    default:
        return false;
    }
}

/// 指令是否**写**某个局部变量槽；是则给出槽位。
bool localWriteSlot(const Insn& in, ca::i32& slot)
{
    if (localKindOf(in.op) != LocalKind::None &&
        !isLocalLoad(in.op)) {          // xSTORE 族
        slot = localSlotOf(in);
        return true;
    }
    if (in.op == Op::IINC) {            // 读改写同一个槽
        slot = in.a;
        return true;
    }
    return false;
}

/// 指令是否**读**某个局部变量槽；是则给出槽位。
bool localReadSlot(const Insn& in, ca::i32& slot)
{
    if (localKindOf(in.op) != LocalKind::None && isLocalLoad(in.op)) {
        slot = localSlotOf(in);
        return true;
    }
    if (in.op == Op::IINC) {
        slot = in.a;
        return true;
    }
    return false;
}

/// long/double 占两个槽；读半边也算读。
int slotSpan(const Insn& in)
{
    const LocalKind k = localKindOf(in.op);
    return (k == LocalKind::Long || k == LocalKind::Double) ? 2 : 1;
}

// ── 基本块划分 ──

/// 块首标记。与 `CodeEditor::blocks()` 同源的 leader 分析：顺序头、跳转 /
/// switch 目标、终止指令的顺序后继。异常表边界在这里不需要——本 pass 只
/// 处理 `exc` 为空的方法。
bool leadersOf(const CodeUnit& cu, std::vector<bool>& isLeader)
{
    const auto&     order = cu.dom.order();
    const ca::usize n     = order.size();
    if (n == 0) return false;

    const Layout lay = layoutOf(cu.dom);
    isLeader.assign(n, false);
    isLeader[0] = true;

    for (ca::usize i = 0; i < n; ++i) {
        const Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) return false;

        const Flow f = flowOf(in->op);
        switch (f) {
        case Flow::Subroutine:
            return false;
        case Flow::Stop:
            if (i + 1 < n) isLeader[i + 1] = true;
            break;
        case Flow::Fall:
            break;
        case Flow::Jump:
        case Flow::Cond: {
            ca::usize t = 0;
            if (!indexOfLabel(cu.dom, in->target, lay, t)) return false;
            if (t < n) isLeader[t] = true;
            if (f == Flow::Cond && i + 1 < n) isLeader[i + 1] = true;
            break;
        }
        case Flow::Switch: {
            ca::usize t = 0;
            if (!indexOfLabel(cu.dom, in->target, lay, t)) return false;
            if (t < n) isLeader[t] = true;
            for (const LabelId l : in->targets) {
                if (!indexOfLabel(cu.dom, l, lay, t)) return false;
                if (t < n) isLeader[t] = true;
            }
            if (i + 1 < n) isLeader[i + 1] = true;
            break;
        }
        }
    }
    return true;
}

// ── 局部变量活跃性 ──

/// 槽位位图：`words` 个 u64，第 i 位 = 槽 i。
struct SlotBits
{
    std::vector<ca::u64> w;

    void reset(ca::usize words) { w.assign(words, 0); }
    bool test(ca::usize i) const
    {
        return i / 64 < w.size() && ((w[i / 64] >> (i % 64)) & 1ull) != 0;
    }
    void set(ca::usize i)
    {
        if (i / 64 < w.size()) w[i / 64] |= (1ull << (i % 64));
    }
    void setRange(ca::usize from, ca::usize span)
    {
        for (ca::usize k = 0; k < span; ++k) set(from + k);
    }
};

/// 一条指令读/写的槽跨度。
void accessSpan(const Insn& in, ca::usize& slot, ca::usize& span, bool& isRead, bool& isWrite)
{
    ca::i32 s = 0;
    isRead    = localReadSlot(in, s);
    isWrite   = localWriteSlot(in, s);
    slot      = static_cast<ca::usize>(s < 0 ? 0 : s);
    span      = static_cast<ca::usize>(slotSpan(in));
}

/// 逐指令的活跃变量分析（`liveIn[i]`：进入第 i 条指令时还活着的槽）。
///
/// 标准反向数据流：`liveIn = use ∪ (liveOut \ def)`，`liveOut = ∪ liveIn[succ]`。
/// 用途只有一个——判断"窗口写下的槽在窗口出口之后还有没有人读"。这比"这个槽
/// 全方法都不被读"精确得多：DashO 的填充常常复用真循环变量的槽（写完立刻被
/// 真代码覆盖），按全局不读判会一律漏掉。
bool computeLiveness(const CodeUnit&             cu,
                     const Graph&                g,
                     ca::usize                   slots,
                     std::vector<SlotBits>&      liveIn)
{
    const auto&     order = cu.dom.order();
    const ca::usize n     = order.size();
    if (n == 0) return false;

    const ca::usize words = (slots + 63) / 64;
    if (words == 0) return false;

    std::vector<SlotBits> use(n), def(n), out(n);
    for (ca::usize i = 0; i < n; ++i) {
        const Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) return false;
        use[i].reset(words);
        def[i].reset(words);
        out[i].reset(words);

        ca::usize slot = 0, span = 1;
        bool      isRead = false, isWrite = false;
        accessSpan(*in, slot, span, isRead, isWrite);
        if (isRead) use[i].setRange(slot, span);
        if (isWrite) def[i].setRange(slot, span);
    }

    liveIn.assign(n, SlotBits{});
    for (auto& b : liveIn) b.reset(words);

    for (ca::usize iter = 0; iter < n + 2; ++iter) {
        bool changed = false;
        for (ca::usize k = n; k-- > 0;) {
            // liveOut = ∪ liveIn[succ]
            SlotBits& o = out[k];
            for (ca::usize w = 0; w < words; ++w) o.w[w] = 0;
            if (k < g.succ.size()) {
                for (const ca::usize v : g.succ[k]) {
                    if (v >= n) continue;   // 方法末尾伪节点：无后继
                    for (ca::usize w = 0; w < words; ++w) o.w[w] |= liveIn[v].w[w];
                }
            }
            // liveIn = use ∪ (liveOut \ def)
            for (ca::usize w = 0; w < words; ++w) {
                const ca::u64 nv = use[k].w[w] | (o.w[w] & ~def[k].w[w]);
                if (nv != liveIn[k].w[w]) {
                    liveIn[k].w[w] = nv;
                    changed        = true;
                }
            }
        }
        if (!changed) break;
    }
    return true;
}

// ── 空转窗口旁路 ──

/// 从 `i` 起找一段**栈自足**的指令窗口，要求它恰好收在一条 `goto` 前面。
///
/// 成功时 `out` 是那条 `goto` 的位置，窗口是 `[i, out)`。
///
/// 四条：
///   1. 窗口内不得有块首（`j > i` 处遇 leader 即停）——有块首就说明有分支能
///      跳进窗口中间，删了那条边会悬垂；
///   2. 每条指令都在栈效应白名单里（见 `deltaOf`）；
///   3. 栈深从 0 起全程非负、在 `goto` 前回到 0——"净效应 0"不够，
///      `pop; aconst_null` 也净 0，但它把入口栈顶换成了 null；
///   4. 窗口非空，且那条 `goto` 的目标**不在 `[i, j]` 内**。
///
/// 第 4 条的块内落点本来已被第 1 条挡住（落点即块首），但 `i` 自身是例外——
/// 块首检查从 `i + 1` 起算。若 `goto` 跳回窗口首，删完窗口它就退化成自环。
///
/// 注意窗口**不必是整块**：DashO 会把填充插在实参求值中间（`this.f(x)` 编译成
/// `aload_0; getstatic x; ldc -1; istore_1; …; goto L`），窗口只能从填充的第一
/// 条起算。删掉窗口后，前一条指令自然落到那条 `goto` 上，语义不变。
bool neutralWindow(jbc::ir::ClassSession& session,
                   const CodeUnit&        cu,
                   const std::vector<bool>& isLeader,
                   ca::usize              i,
                   ca::usize&             out)
{
    const auto&     order = cu.dom.order();
    const ca::usize n     = order.size();
    const Layout    lay   = layoutOf(cu.dom);

    int       depth = 0;
    ca::usize j     = i;
    for (; j < n; ++j) {
        if (j > i && isLeader[j]) return false;   // 有人跳进窗口中间

        const Insn* in = cu.dom.peek(order[j]);
        if (in == nullptr) return false;
        if (in->op == Op::GOTO || in->op == Op::GOTO_W) {
            ca::usize t = 0;
            if (!indexOfLabel(cu.dom, in->target, lay, t)) return false;
            if (t >= i && t <= j) return false;   // 跳回窗口内（含自环）
            break;                                 // 窗口到此为止
        }

        Delta d;
        if (!deltaOf(session, *in, d)) return false;
        if (depth < d.pop) return false;          // 吃了入口栈上的值
        depth += d.push - d.pop;
    }

    if (j >= n || j == i || depth != 0) return false;

    out = j;
    return true;
}

/// 旁路"栈自足 + 写下的槽在出口后已死 + 收在 goto 前"的窗口。
///
/// 做法是**只删窗口、留下末尾的 goto**：`kRetargetToFallthrough` 会把所有跳向
/// 窗口首的边改锚到窗口之后的第一个存活指令（就是那条 goto），于是窗口位置
/// 退化成一条孤立 goto；随后 `threadGotos` 把指向它的边接到它自己的目标，
/// `pruneUnreachable` 再把这条孤立 goto 收走。这样每一步都只用库已有的原子
/// 操作，不需要"把 A 的所有前驱改指 B"这种要自己维护边表的动作。
ca::usize bypassNeutralRuns(jbc::ir::ClassSession& session, CodeUnit& cu)
{
    std::vector<bool> isLeader;
    if (!leadersOf(cu, isLeader)) return 0;

    Graph g;
    if (!buildGraph(cu, g)) return 0;

    const auto&     order = cu.dom.order();
    const ca::usize n     = order.size();

    ca::usize slots = cu.maxLocals;
    for (ca::usize i = 0; i < n; ++i) {
        const Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) return 0;
        ca::usize slot = 0, span = 1;
        bool      r = false, wr = false;
        accessSpan(*in, slot, span, r, wr);
        if (slot + span > slots) slots = slot + span;
    }
    if (slots == 0) return 0;

    std::vector<SlotBits> liveIn;
    if (!computeLiveness(cu, g, slots, liveIn)) return 0;

    std::vector<InsnId> victims;

    ca::usize i = 0;
    while (i < n) {
        ca::usize end = 0;
        if (!neutralWindow(session, cu, isLeader, i, end)) {
            ++i;
            continue;
        }

        // 判据 2：窗口写下的每个槽，在 goto 处都必须是死的。
        bool writesLive = false;
        for (ca::usize k = i; k < end && !writesLive; ++k) {
            const Insn* in = cu.dom.peek(order[k]);
            ca::usize   slot = 0, span = 1;
            bool        r = false, wr = false;
            accessSpan(*in, slot, span, r, wr);
            if (!wr) continue;
            for (ca::usize s = 0; s < span; ++s) {
                if (liveIn[end].test(slot + s)) {
                    writesLive = true;
                    break;
                }
            }
        }
        if (writesLive) {
            ++i;
            continue;
        }

        for (ca::usize k = i; k < end; ++k) victims.push_back(order[k]);
        i = end + 1;   // 跳过那条 goto（它要留着）
    }

    if (victims.empty()) return 0;

    jbc::ir::CodeEditor editor(cu);
    if (editor.eraseBatch(victims, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
            .is_err()) {
        return 0;   // 有异常表边界或落点缺失：整体放弃，不做半截
    }
    return victims.size();
}

// ── goto 线程化 ──

/// 把"跳到一条 goto"的边接到那条 goto 自己的目标上。
///
/// 块切分把一条长跳拆成 `goto A` / `A: goto B` 这样的接力，反编译器会为每一
/// 跳造一层中转。顺着 goto 链走到头再一次性改写，比逐跳改快且不会来回震荡
/// （`goto A` / `A: goto B` / `B: goto A` 这种环也能停在第一次重复处）。
///
/// switch 的 case 表不动——`setTarget` 只改默认目标，改了会让默认分支和 case
/// 分支指向不同的中转块，形态更差；反正两边最终落到同一处，不改也不影响语义。
ca::usize threadGotos(CodeUnit& cu)
{
    const auto&     order = cu.dom.order();
    const ca::usize n     = order.size();
    const Layout    lay   = layoutOf(cu.dom);

    struct Rewire
    {
        InsnId at;
        LabelId to;
    };
    std::vector<Rewire> plan;

    std::vector<bool> visited;
    for (ca::usize i = 0; i < n; ++i) {
        const Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) continue;

        const Flow f = flowOf(in->op);
        if (f != Flow::Jump && f != Flow::Cond) continue;

        ca::usize t = 0;
        if (!indexOfLabel(cu.dom, in->target, lay, t)) continue;

        visited.assign(n, false);
        LabelId finalTarget = in->target;
        bool    moved       = false;

        for (ca::usize guard = 0; guard < n; ++guard) {
            if (t >= n || visited[t]) break;
            visited[t] = true;

            const Insn* dst = cu.dom.peek(order[t]);
            if (dst == nullptr) break;
            if (dst->op != Op::GOTO && dst->op != Op::GOTO_W) break;

            ca::usize t2 = 0;
            if (!indexOfLabel(cu.dom, dst->target, lay, t2)) break;
            if (t2 == t) break;   // 自环

            finalTarget = dst->target;
            t           = t2;
            moved       = true;
        }

        if (moved) plan.push_back(Rewire{order[i], finalTarget});
    }

    if (plan.empty()) return 0;

    jbc::ir::CodeEditor editor(cu);
    ca::usize           done = 0;
    for (const Rewire& r : plan) {
        if (editor.setTarget(r.at, r.to).is_ok()) ++done;
    }
    return done;
}

}   // namespace

mjbase::spi::PreservedAnalyses DashoCleanupPass::run(mjbase::model::ResourceContainer& container,
                                                    mjbase::spi::GlobalAnalysisManager& am)
{
    static_cast<void>(am);

    ClassSet set;
    set.load(container);
    if (set.classes().empty()) return mjbase::spi::PreservedAnalyses::all();

    ca::usize cleanedMethods = 0;
    ca::usize removedInsns   = 0;
    ca::usize dirtyClasses   = 0;
    ca::usize skippedWithExc = 0;

    for (LoadedClass& lc : set.classes()) {
        const ca::usize n = lc.session->unit().methods.size();
        for (ca::usize i = 0; i < n; ++i) {
            if (!jbc::ir::hasCode(*lc.session, i)) continue;

            auto promoted = jbc::ir::promoteMethod(*lc.session, i);
            if (promoted.is_err()) continue;
            jbc::ir::CodeUnitRef ref = std::move(promoted).unwrap();
            CodeUnit&            cu  = *ref;

            // 有真 handler 的方法整体跳过：删一条指令就可能让 handler 体变成
            // "只被异常边可达"，而帧再生只给可达目标发帧，留下死块即
            // `VerifyError: Expecting a stack map frame`。见头文件注释。
            if (!cu.exc.empty()) {
                ++skippedWithExc;
                continue;
            }

            ca::usize total = 0;
            for (ca::usize round = 0; round < kMaxRounds; ++round) {
                ca::usize step = 0;
                step += pruneUnreachable(cu);
                step += bypassNeutralRuns(*lc.session, cu);
                step += threadGotos(cu);
                step += tidyGotoToNext(cu);
                total += step;
                if (step == 0) break;
            }
            // 溢写对折叠放在最后：它只处理"直接相邻"的两条，前面几步把中间
            // 的 goto / 死块清掉之后，原本被隔开的一对才浮出来。
            total += foldStoreLoadPairs(cu);

            if (total > 0) {
                lc.dirty.push_back(DirtyMethod{i, true});
                ++cleanedMethods;
                removedInsns += total;
            }
        }
        if (!lc.dirty.empty()) ++dirtyClasses;
    }

    if (cleanedMethods == 0) {
        CA_LOG_INFO("[dasho.Cleanup] 未发现可清除的控制流填充");
        return mjbase::spi::PreservedAnalyses::all();
    }

    const ca::usize written = set.commit(container);
    CA_LOG_INFO("[dasho.Cleanup] 清理 {} 个方法（删除 {} 条指令），写回 {} 个类"
                "（{} 个带异常表的方法跳过）",
                cleanedMethods, removedInsns, written, skippedWithExc);
    return mjbase::spi::PreservedAnalyses::none();
}

Result<void, std::string> registerCleanup(mjbase::spi::PassNameRegistry& reg)
{
    if (Result<void, mjbase::spi::RegistryError> r = reg.registerGlobalPass(
            "dasho.Cleanup",
            [] { return std::make_unique<DashoCleanupPass>(); },
            "Remove DashO control-flow padding: unreachable fill blocks, stack-neutral"
            " dead-store blocks (ldc -1; istore N; ldc \"0\"; iinc N 1; astore N+1; goto L),"
            " goto chains and store/load spill pairs. Methods that still carry an exception"
            " table are skipped wholesale.");
        r.is_err()) {
        return Err(std::string("dasho.Cleanup"));
    }
    return Ok();
}

}   // namespace dasho
