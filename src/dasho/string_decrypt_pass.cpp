#include "dasho/string_decrypt_pass.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jstr/conversion.hpp"
#include "jstr/mutf8_string.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/model/resource_model.hpp"

#include "dasho/class_set.hpp"
#include "dasho/dead_class.hpp"
#include "dasho/jvm_interp.hpp"

namespace dasho {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;

namespace {

// ==== SCAN ====================================================================
// 找出"这个调用点是不是一个可静态求值的字符串解密站点"。纯读。
//
//   粗找  区间回溯窗口 kRegionWindow 内找 invokestatic 锚点        可钉形状
//   细找  区间栈自足 + 解密器纯 + 求值器跑出合法明文
//         判据等级 **S2**（结构事实）——跨构建稳定，跨 DashO 版本需复核
//   拒绝  解密器不纯 / 在 jar 外 / 求值失败 / 明文非法 / 局部变量外泄
//
// 关键设计：**不依赖解密器的名字、签名或所在类**，所以换一个加密 seed 或换一个
// 运行时类名都不影响本段——判据钉的是"调用点可静态求值"这个结构事实。
//
// 求值器的多 seed 校验（kSeeds）是拒绝面的一部分：Random 种子不同而结果相同，
// 才认定该站点与随机源无关；否则拒绝，因为它依赖运行时状态。

/// 区间回溯窗口：DashO 调用点（密钥算术 + 谓词 + 双支 + 解密链）实测不超过
/// 40 条指令；留一倍余量，避免在超长方法体上做无界回溯。
constexpr ca::usize kRegionWindow = 80;

/// 多 seed 求值：Random 种子不同而结果相同，才认定该站点与随机源无关。
constexpr ca::u64 kSeeds[] = {0x2545F4914F6CDD1DULL, 0x9E3779B97F4A7C15ULL,
                              0xDEADBEEFCAFEBABEULL};

/// 「无此下标」哨兵（order 下标域，区别于 InsnId 域的 kNoInsn）。
constexpr ca::usize kNoPos = static_cast<ca::usize>(-1);

/// 站点级诊断开关（`DASHO_TRACE=1`）：只在排查「为什么这个站点没折叠」时开，
/// 平时零开销（首次调用后只读一个静态量）。
bool traceOn()
{
    static const bool on = [] {
        const char* v = std::getenv("DASHO_TRACE");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

/// 诊断输出走 stderr 而非日志通道：单测不装配日志汇，站点级排查要两条路径
/// 都能看到同一条原因。
#define DASHO_TRACE_REJECT(why)                                                     \
    do {                                                                            \
        if (traceOn()) {                                                            \
            std::fprintf(stderr, "[dasho.trace] reject @start=%zu anchor=%zu: %s\n", \
                         static_cast<size_t>(start), static_cast<size_t>(anchor),    \
                         (why));                                                     \
        }                                                                           \
    } while (0)

/// 明文合法性兜底：拒 NUL、拒孤立代理项、拒 C0/C1 控制符（\t\n\r 放行）。
/// 错误求值极少能同时命中多 seed 一致与明文合法两道闸。
bool plainUtf16Ok(const std::u16string& s)
{
    for (ca::usize i = 0; i < s.size(); ++i) {
        const char16_t c = s[i];
        if (c == 0) return false;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= s.size()) return false;
            const char16_t n = s[i + 1];
            if (n < 0xDC00 || n > 0xDFFF) return false;
            ++i;
        }
        else if (c >= 0xDC00 && c <= 0xDFFF) {
            return false;
        }
        else if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) {
            return false;
        }
        else if (c >= 0x7F && c <= 0x9F) {
            return false;
        }
    }
    return true;
}

bool utf16ToMutf8(const std::u16string& s, std::string& out)
{
    try {
        std::vector<ca::u16> units(s.begin(), s.end());
        const ca::str::Utf16String utf16(units.data(), units.size());
        const std::string          utf8 = utf16.to_utf8_string().ref().to_std_string();
        const auto                 mutf8 = jstr::utf8_to_mutf8(utf8);
        if (mutf8.is_err()) return false;
        out = mutf8.unwrap().bytes();
    }
    catch (...) {
        return false;
    }
    return true;
}

bool mutf8ToUtf16(std::string_view mutf8, std::u16string& out)
{
    const jstr::MUtf8StringRef src(mutf8.data(), mutf8.size());
    const auto                 utf8Result = jstr::mutf8_to_utf8(src);
    if (utf8Result.is_err()) return false;
    try {
        const std::string& utf8 = utf8Result.unwrap();
        const ca::str::Utf16String u16 = ca::str::Utf16String::from_utf8_string(
            ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(utf8.data()),
                                              utf8.size()));
        out.clear();
        out.reserve(u16.length());
        for (ca::usize i = 0; i < u16.length(); ++i) {
            out.push_back(static_cast<ca::u16>(u16.char_at(i).value));
        }
    }
    catch (...) {
        return false;
    }
    return true;
}

/// 引用值 → UTF-16 视图（Str 与已构造的 String 实例同义）。
const std::u16string* strOfValue(const Value& v)
{
    if (v.kind == Value::Kind::Str) return &v.s;
    if (v.kind == Value::Kind::Obj && v.obj && v.obj->kind == HeapObj::Kind::StringVal) {
        return &v.obj->text;
    }
    return nullptr;
}

/// 描述符参数表：每项标记该位置是否为 java/lang/String。
///
/// 原地改写只关心「哪一位是密文」，不关心具体类型——收栈序列只区分 cat-1
/// 与 cat-2，而 cat-2（long/double）一律放弃（swap 对 cat-2 无定义）。
bool parseParams(std::string_view desc, std::vector<bool>& isString)
{
    isString.clear();
    const auto open = desc.find('(');
    if (open == std::string_view::npos) return false;
    ca::usize i = open + 1;
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        switch (c) {
        case 'B': case 'C': case 'D': case 'F': case 'I': case 'J': case 'S': case 'Z':
            isString.push_back(false);
            ++i;
            break;
        case 'L': {
            const auto semi = desc.find(';', i);
            if (semi == std::string_view::npos) return false;
            isString.push_back(desc.substr(i, semi - i + 1) == "Ljava/lang/String;");
            i = semi + 1;
            break;
        }
        case '[':
            // 数组是引用类型，槽位恒为 1；跳过元素类型。
            while (i < desc.size() && desc[i] == '[') ++i;
            if (i >= desc.size()) return false;
            if (desc[i] == 'L') {
                const auto semi = desc.find(';', i);
                if (semi == std::string_view::npos) return false;
                i = semi + 1;
            }
            else {
                ++i;
            }
            isString.push_back(false);
            break;
        default:
            return false;
        }
    }
    return i < desc.size() && desc[i] == ')';
}

/// 描述符的栈槽用量（实参槽数 / 返回槽数）。
bool descSlots(std::string_view desc, ca::i32& argSlots, ca::i32& retSlots)
{
    argSlots = 0;
    retSlots = 0;
    const auto open  = desc.find('(');
    const auto close = desc.find(')');
    if (open == std::string_view::npos || close == std::string_view::npos) return false;
    for (ca::usize i = open + 1; i < close; ++i) {
        switch (desc[i]) {
        case 'J': case 'D':
            argSlots += 2;
            break;
        case 'L': {
            const auto semi = desc.find(';', i);
            if (semi == std::string_view::npos || semi > close) return false;
            argSlots += 1;
            i = semi;
            break;
        }
        case '[':
            while (i < close && desc[i] == '[') ++i;
            if (i >= close) return false;
            if (desc[i] == 'L') {
                const auto semi = desc.find(';', i);
                if (semi == std::string_view::npos || semi > close) return false;
                i = semi;
            }
            argSlots += 1;
            break;
        case 'B': case 'C': case 'F': case 'I': case 'S': case 'Z':
            argSlots += 1;
            break;
        default:
            return false;
        }
    }
    const std::string_view ret = desc.substr(close + 1);
    if (ret.empty()) return false;
    if (ret == "V") retSlots = 0;
    else if (ret[0] == 'J' || ret[0] == 'D') retSlots = 2;
    else retSlots = 1;
    return true;
}

/// 单条指令的栈槽增量（push − pop）；不可判定返回 false。
///
/// 只覆盖「常量折叠区间」里可能出现的形态：常量装载、局部变量读写、算术、
/// 转换、比较、栈操作、分支、invoke、new/数组。字段访问、jsr/ret、indy、
/// athrow 等一律不可判定——它们本来也进不了求值成功的区间。
bool stackDelta(ClassSession& session, const Insn& in, ca::i32& out)
{
    switch (in.op) {
    // ── 零增量 ──
    case Op::NOP: case Op::IINC: case Op::SWAP: case Op::CHECKCAST:
    case Op::INSTANCEOF: case Op::ARRAYLENGTH: case Op::NEWARRAY: case Op::ANEWARRAY:
    case Op::INEG: case Op::LNEG: case Op::FNEG: case Op::DNEG:
    case Op::I2F: case Op::I2B: case Op::I2C: case Op::I2S:
    case Op::F2I: case Op::L2D: case Op::D2L:
    case Op::GOTO: case Op::GOTO_W: case Op::RETURN:
    case Op::LALOAD: case Op::DALOAD:
        out = 0;
        return true;

    // ── 压 1 槽 ──
    case Op::ACONST_NULL:
    case Op::ICONST_M1: case Op::ICONST_0: case Op::ICONST_1: case Op::ICONST_2:
    case Op::ICONST_3: case Op::ICONST_4: case Op::ICONST_5:
    case Op::BIPUSH: case Op::SIPUSH: case Op::LDC: case Op::LDC_W:
    case Op::ILOAD: case Op::FLOAD: case Op::ALOAD:
    case Op::ILOAD_0: case Op::ILOAD_1: case Op::ILOAD_2: case Op::ILOAD_3:
    case Op::FLOAD_0: case Op::FLOAD_1: case Op::FLOAD_2: case Op::FLOAD_3:
    case Op::ALOAD_0: case Op::ALOAD_1: case Op::ALOAD_2: case Op::ALOAD_3:
    case Op::DUP: case Op::DUP_X1: case Op::DUP_X2:
    case Op::I2L: case Op::I2D: case Op::F2L: case Op::F2D:
    case Op::NEW:
        out = 1;
        return true;

    // ── 压 2 槽 ──
    case Op::LDC2_W:
    case Op::LLOAD: case Op::DLOAD:
    case Op::LLOAD_0: case Op::LLOAD_1: case Op::LLOAD_2: case Op::LLOAD_3:
    case Op::DLOAD_0: case Op::DLOAD_1: case Op::DLOAD_2: case Op::DLOAD_3:
    case Op::DUP2: case Op::DUP2_X1: case Op::DUP2_X2:
        out = 2;
        return true;

    // ── 弹 1 槽 ──
    case Op::POP:
    case Op::ISTORE: case Op::FSTORE: case Op::ASTORE:
    case Op::ISTORE_0: case Op::ISTORE_1: case Op::ISTORE_2: case Op::ISTORE_3:
    case Op::FSTORE_0: case Op::FSTORE_1: case Op::FSTORE_2: case Op::FSTORE_3:
    case Op::ASTORE_0: case Op::ASTORE_1: case Op::ASTORE_2: case Op::ASTORE_3:
    case Op::IALOAD: case Op::FALOAD: case Op::AALOAD: case Op::BALOAD:
    case Op::CALOAD: case Op::SALOAD:
    case Op::IADD: case Op::FADD: case Op::ISUB: case Op::FSUB:
    case Op::IMUL: case Op::FMUL: case Op::IDIV: case Op::FDIV:
    case Op::IREM: case Op::FREM:
    case Op::ISHL: case Op::ISHR: case Op::IUSHR:
    case Op::IAND: case Op::IOR: case Op::IXOR:
    case Op::L2I: case Op::L2F: case Op::D2I: case Op::D2F:
    case Op::FCMPL: case Op::FCMPG:
    case Op::IFEQ: case Op::IFNE: case Op::IFLT: case Op::IFGE:
    case Op::IFGT: case Op::IFLE: case Op::IFNULL: case Op::IFNONNULL:
    case Op::TABLESWITCH: case Op::LOOKUPSWITCH:
    case Op::IRETURN: case Op::FRETURN: case Op::ARETURN:
    case Op::MONITORENTER: case Op::MONITOREXIT:
        out = -1;
        return true;

    // ── 弹 2 槽 ──
    case Op::POP2:
    case Op::LSTORE: case Op::DSTORE:
    case Op::LSTORE_0: case Op::LSTORE_1: case Op::LSTORE_2: case Op::LSTORE_3:
    case Op::DSTORE_0: case Op::DSTORE_1: case Op::DSTORE_2: case Op::DSTORE_3:
    case Op::LADD: case Op::DADD: case Op::LSUB: case Op::DSUB:
    case Op::LMUL: case Op::DMUL: case Op::LDIV: case Op::DDIV:
    case Op::LREM: case Op::DREM:
    case Op::LSHL: case Op::LSHR: case Op::LUSHR:
    case Op::LAND: case Op::LOR: case Op::LXOR:
    case Op::IF_ICMPEQ: case Op::IF_ICMPNE: case Op::IF_ICMPLT:
    case Op::IF_ICMPGE: case Op::IF_ICMPGT: case Op::IF_ICMPLE:
    case Op::IF_ACMPEQ: case Op::IF_ACMPNE:
    case Op::LRETURN: case Op::DRETURN:
        out = -2;
        return true;

    // ── 弹 3 槽 ──
    case Op::IASTORE: case Op::FASTORE: case Op::AASTORE: case Op::BASTORE:
    case Op::CASTORE: case Op::SASTORE:
    case Op::LCMP: case Op::DCMPL: case Op::DCMPG:
        out = -3;
        return true;

    case Op::LASTORE: case Op::DASTORE:
        out = -4;
        return true;

    case Op::MULTIANEWARRAY:
        out = 1 - in.a;
        return true;

    case Op::INVOKESTATIC: case Op::INVOKEVIRTUAL: case Op::INVOKESPECIAL:
    case Op::INVOKEINTERFACE: {
        ClassSession::MemberSem sem;
        if (!session.memberOf(in, sem)) return false;
        ca::i32 argSlots = 0;
        ca::i32 retSlots = 0;
        if (!descSlots(sem.desc, argSlots, retSlots)) return false;
        const bool receiver = in.op != Op::INVOKESTATIC;
        out = retSlots - argSlots - (receiver ? 1 : 0);
        return true;
    }

    default:
        return false;
    }
}

/// 描述符返回值是否为 String。
bool returnsString(std::string_view desc)
{
    const auto close = desc.find(')');
    if (close == std::string_view::npos) return false;
    return desc.substr(close + 1) == "Ljava/lang/String;";
}

/// 描述符参数里是否含 String（解密器的密文实参）。
bool takesString(std::string_view desc)
{
    const auto open  = desc.find('(');
    const auto close = desc.find(')');
    if (open == std::string_view::npos || close == std::string_view::npos) return false;
    return desc.substr(open, close - open).find("Ljava/lang/String;") != std::string_view::npos;
}

/// 死代码可擦除判据：区间内未被执行的指令必须是纯计算/纯调用。
/// 出现字段访问、同步、athrow 等有副作用形态即放弃整个区间。
bool erasableOp(const Insn& in)
{
    switch (in.op) {
    case Op::GETSTATIC: case Op::PUTSTATIC: case Op::GETFIELD: case Op::PUTFIELD:
    case Op::MONITORENTER: case Op::MONITOREXIT: case Op::ATHROW:
    case Op::INVOKEDYNAMIC: case Op::JSR: case Op::JSR_W: case Op::RET:
        return false;
    default:
        return true;
    }
}

bool isLoadOp(const Op op)
{
    switch (op) {
    case Op::ILOAD: case Op::LLOAD: case Op::FLOAD: case Op::DLOAD: case Op::ALOAD:
    case Op::ILOAD_0: case Op::ILOAD_1: case Op::ILOAD_2: case Op::ILOAD_3:
    case Op::LLOAD_0: case Op::LLOAD_1: case Op::LLOAD_2: case Op::LLOAD_3:
    case Op::FLOAD_0: case Op::FLOAD_1: case Op::FLOAD_2: case Op::FLOAD_3:
    case Op::DLOAD_0: case Op::DLOAD_1: case Op::DLOAD_2: case Op::DLOAD_3:
    case Op::ALOAD_0: case Op::ALOAD_1: case Op::ALOAD_2: case Op::ALOAD_3:
        return true;
    default:
        return false;
    }
}

bool isStoreOp(const Op op)
{
    switch (op) {
    case Op::ISTORE: case Op::LSTORE: case Op::FSTORE: case Op::DSTORE: case Op::ASTORE:
    case Op::ISTORE_0: case Op::ISTORE_1: case Op::ISTORE_2: case Op::ISTORE_3:
    case Op::LSTORE_0: case Op::LSTORE_1: case Op::LSTORE_2: case Op::LSTORE_3:
    case Op::FSTORE_0: case Op::FSTORE_1: case Op::FSTORE_2: case Op::FSTORE_3:
    case Op::DSTORE_0: case Op::DSTORE_1: case Op::DSTORE_2: case Op::DSTORE_3:
    case Op::ASTORE_0: case Op::ASTORE_1: case Op::ASTORE_2: case Op::ASTORE_3:
        return true;
    default:
        return false;
    }
}

ca::i32 localSlotOf(const Insn& in)
{
    switch (in.op) {
    case Op::ILOAD_0: case Op::LLOAD_0: case Op::FLOAD_0: case Op::DLOAD_0: case Op::ALOAD_0:
    case Op::ISTORE_0: case Op::LSTORE_0: case Op::FSTORE_0: case Op::DSTORE_0: case Op::ASTORE_0:
        return 0;
    case Op::ILOAD_1: case Op::LLOAD_1: case Op::FLOAD_1: case Op::DLOAD_1: case Op::ALOAD_1:
    case Op::ISTORE_1: case Op::LSTORE_1: case Op::FSTORE_1: case Op::DSTORE_1: case Op::ASTORE_1:
        return 1;
    case Op::ILOAD_2: case Op::LLOAD_2: case Op::FLOAD_2: case Op::DLOAD_2: case Op::ALOAD_2:
    case Op::ISTORE_2: case Op::LSTORE_2: case Op::FSTORE_2: case Op::DSTORE_2: case Op::ASTORE_2:
        return 2;
    case Op::ILOAD_3: case Op::LLOAD_3: case Op::FLOAD_3: case Op::DLOAD_3: case Op::ALOAD_3:
    case Op::ISTORE_3: case Op::LSTORE_3: case Op::FSTORE_3: case Op::DSTORE_3: case Op::ASTORE_3:
        return 3;
    default:
        return in.a;
    }
}

/// 单方法内的改写器：持会话与 CodeUnit，负责区间校验、求值与替换。
///
/// 约定允许的**例外形态**（CONVENTION.md §5.3）：`.cpp` 内的自由 struct/class 只是
/// 局部变量的组织方式，不是接口——它不进头文件、不可继承、adapter 看不见它。
/// 判据很简单：出现在 `.hpp` 里就是接口，出现在 `.cpp` 里就是局部变量。
///
/// 本类同时装着 SCAN 方法与 REWRITE 方法，分区标记照常打在各方法上方。
///
/// 两种改写策略，按方法是否带异常表选择：
///   - **整段折叠**（无异常表）：区间替换为一条 ldc 明文，死支诱饵一并消失。
///     指令增删 → 既有帧目标悬垂 → 写回时按新布局再生 StackMapTable。
///   - **原地改写**（有异常表）：只把密文 ldc 换成明文、把解密调用换成等价的
///     栈操作（nop / pop / swap;pop）。指令数与栈形不变 → 原帧（含 verifier
///     要求的保守 handler 帧）逐条仍然有效，不需要也不可能安全地重算。
///     代价是死支诱饵留在原地（不可达，但仍是可见噪声）。
class MethodFolder
{
public:
    MethodFolder(ClassSet& set, LoadedClass& lc, CodeUnit& unit)
        : set_(set), lc_(lc), unit_(unit)
    {}

    /// 反复扫描直到不动点；返回折叠站点数。
    ///
    /// **锚点按倒序扫描（最外层优先）**：嵌套调用 `f(g(x))` 里外层的
    /// invokestatic 一定在内层之后。若先折内层，内层的收栈序列会把实参
    /// 压成 1 个值，而外层区域仍按「原始实参个数」改写——两条路径的栈深
    /// 不一致，产物必然 VerifyError。先折外层，内层作为区域的一部分被
    /// 常量折叠吸收，不存在嵌套残留。
    ca::usize run()
    {
        ca::usize folded = 0;
        bool      progress = true;
        int       rounds = 0;
        const int maxRounds = static_cast<int>(unit_.dom.order().size()) + 8;
        while (progress && rounds++ < maxRounds) {
            progress = false;
            const ca::usize n = unit_.dom.order().size();
            for (ca::usize c = n; c-- > 0;) {
                if (!isAnchor(c)) continue;
                noteCallee(c);
                if (tryFoldAt(c)) {
                    ++folded;
                    progress = true;
                    break;   // 指令序列已变，重扫
                }
            }
        }
        return folded;
    }

    /// 本方法的改写是否增删了指令（决定写回时是否需要再生帧）。
    bool structural() const noexcept { return structural_; }

    /// 候选锚点解析到的解密器类（内部名）。只用于事后清扫死解密器类，
    /// 多记几个不影响正确性：真被引用的类扫不掉。
    const std::unordered_set<std::string>& callees() const noexcept { return callees_; }

private:
    /// 记录该锚点指向的类（见 callees() 注记）。
    void noteCallee(ca::usize idx)
    {
        const Insn* in = unit_.dom.peek(unit_.dom.order()[idx]);
        if (in == nullptr) return;
        ClassSession::MemberSem sem;
        if (!lc_.session->memberOf(*in, sem)) return;
        if (!returnsString(sem.desc)) return;
        callees_.emplace(sem.owner.data(), sem.owner.size());
    }

    bool isAnchor(ca::usize idx) const
    {
        const Insn* in = unit_.dom.peek(unit_.dom.order()[idx]);
        if (in == nullptr || in->op != Op::INVOKESTATIC) return false;
        ClassSession::MemberSem sem;
        if (!lc_.session->memberOf(*in, sem)) return false;
        if (!returnsString(sem.desc) || !takesString(sem.desc)) return false;
        ClassSession* target = set_.find(sem.owner);
        if (target == nullptr) return false;
        const auto& methods = target->unit().methods;
        for (ca::usize i = 0; i < methods.size(); ++i) {
            if (target->unit().symbols().str(methods[i].name) == sem.name &&
                target->unit().symbols().str(methods[i].desc) == sem.desc)
            {
                return set_.isPure(*target, i);
            }
        }
        return false;
    }

    /// 区间结构校验：外部不得跳入区间内部，异常表边界不得落在区间内部。
    bool structurallyOk(ca::usize start, ca::usize end) const
    {
        const auto& order = unit_.dom.order();
        const auto  inside = [&](ca::usize i) { return i > start && i <= end; };

        auto indexOf = [&](InsnId id, ca::usize& out) -> bool {
            for (ca::usize i = 0; i < order.size(); ++i) {
                if (order[i] == id) {
                    out = i;
                    return true;
                }
            }
            return false;
        };

        for (ca::usize i = 0; i < order.size(); ++i) {
            if (i >= start && i <= end) continue;
            const Insn* in = unit_.dom.peek(order[i]);
            if (in == nullptr) return false;
            auto checkLabel = [&](jbc::ir::LabelId lbl) -> bool {
                const auto r = unit_.dom.resolveLabel(lbl);
                if (r.is_err()) return false;
                const jbc::ir::LabelTarget t = r.unwrap();
                if (t.isEnd()) return true;
                ca::usize ti = 0;
                if (!indexOf(t.insn, ti)) return false;
                return !inside(ti);
            };
            switch (in->op) {
            case Op::GOTO: case Op::GOTO_W: case Op::IFEQ: case Op::IFNE: case Op::IFLT:
            case Op::IFGE: case Op::IFGT: case Op::IFLE: case Op::IF_ICMPEQ: case Op::IF_ICMPNE:
            case Op::IF_ICMPLT: case Op::IF_ICMPGE: case Op::IF_ICMPGT: case Op::IF_ICMPLE:
            case Op::IF_ACMPEQ: case Op::IF_ACMPNE: case Op::IFNULL: case Op::IFNONNULL:
            case Op::JSR: case Op::JSR_W:
                if (!checkLabel(in->target)) return false;
                break;
            case Op::TABLESWITCH: case Op::LOOKUPSWITCH:
                if (!checkLabel(in->target)) return false;
                for (const jbc::ir::LabelId lbl : in->targets) {
                    if (!checkLabel(lbl)) return false;
                }
                break;
            default:
                break;
            }
        }

        for (const jbc::ir::CodeUnit::Exc& e : unit_.exc) {
            for (const jbc::ir::LabelId lbl : {e.start, e.end, e.handler}) {
                const auto r = unit_.dom.resolveLabel(lbl);
                if (r.is_err()) return false;
                const jbc::ir::LabelTarget t = r.unwrap();
                if (t.isEnd()) continue;
                ca::usize ti = 0;
                if (!indexOf(t.insn, ti)) return false;
                if (inside(ti)) return false;
            }
        }
        return true;
    }

    bool containsStringLdc(ca::usize start, ca::usize end) const
    {
        const auto& order = unit_.dom.order();
        for (ca::usize i = start; i <= end; ++i) {
            const Insn* in = unit_.dom.peek(order[i]);
            if (in == nullptr) continue;
            if (in->op != Op::LDC && in->op != Op::LDC_W) continue;
            ClassSession::ConstSem cs;
            if (lc_.session->constOf(*in, cs) &&
                cs.kind == ClassSession::ConstSem::Kind::String)
            {
                return true;
            }
        }
        return false;
    }

    /// 区间写入的局部变量不得在区间外被读取（否则擦除会丢值）。
    bool localsNotEscaping(ca::usize start, ca::usize end) const
    {
        const auto& order = unit_.dom.order();
        std::vector<ca::i32> written;
        for (ca::usize i = start; i <= end; ++i) {
            const Insn* in = unit_.dom.peek(order[i]);
            if (in != nullptr && isStoreOp(in->op)) written.push_back(localSlotOf(*in));
        }
        if (written.empty()) return true;
        for (ca::usize i = 0; i < order.size(); ++i) {
            if (i >= start && i <= end) continue;
            const Insn* in = unit_.dom.peek(order[i]);
            if (in == nullptr || !isLoadOp(in->op)) continue;
            const ca::i32 slot = localSlotOf(*in);
            if (std::find(written.begin(), written.end(), slot) != written.end()) return false;
        }
        return true;
    }

    /// 区间求值：多 seed 一致 + 栈净 +1 + 明文合法。
    bool evaluate(ca::usize start, ca::usize end, std::u16string& plain,
                  std::vector<bool>& executed) const
    {
        bool first = true;
        for (const ca::u64 seed : kSeeds) {
            Interp interp(set_.map(),
                          [this](ClassSession& s, ca::usize i) { return set_.isPure(s, i); },
                          seed);
            std::vector<bool> local;
            auto r = interp.runRegion(*lc_.session, unit_, start, end, &local);
            if (r.is_err()) {
                if (traceOn()) {
                    std::fprintf(stderr, "[dasho.trace]   interp fail @%zu..%zu: %s\n",
                                 static_cast<size_t>(start), static_cast<size_t>(end),
                                 to_string(r.unwrap_err()));
                }
                return false;
            }
            const std::vector<Value>& stack = r.unwrap();
            if (stack.size() != 1) {
                if (traceOn()) {
                    std::fprintf(stderr, "[dasho.trace]   stack size %zu != 1\n",
                                 static_cast<size_t>(stack.size()));
                }
                return false;
            }
            const Value& v = stack[0];
            std::u16string text;
            if (v.kind == Value::Kind::Str) {
                text = v.s;
            }
            else if (v.kind == Value::Kind::Obj && v.obj &&
                     v.obj->kind == HeapObj::Kind::StringVal)
            {
                text = v.obj->text;
            }
            else {
                return false;
            }
            if (first) {
                plain    = std::move(text);
                executed = std::move(local);
                first    = false;
            }
            else if (plain != text) {
                return false;   // 结果依赖随机源 → 非纯常量折叠
            }
        }
        return !first;
    }

    /// 到达 anchor 的栈深是否唯一（区间内所有路径收敛到同一深度）。
    ///
    /// 原地改写用「弹掉其余实参、留下密文」的收栈序列替代解密调用，只有当
    /// 每条路径到达锚点时栈深一致，这个序列才在每条路径上都成立。DashO 的
    /// 双支模板两支各压 1 个值，天然满足；非对称分支（如嵌套调用被折叠后
    /// 残留）会在这里被挡下，改写退化为「不改」而不是产出坏字节码。
    bool anchorDepthUniform(ca::usize start, ca::usize anchor, ca::i32& depthOut) const
    {
        const auto&          order = unit_.dom.order();
        std::vector<ca::i32> depth(order.size(), -1);
        std::vector<ca::usize> work;
        depth[start] = 0;
        work.push_back(start);

        auto targetPos = [&](jbc::ir::LabelId lbl, ca::usize& out) -> bool {
            const auto r = unit_.dom.resolveLabel(lbl);
            if (r.is_err()) return false;
            const jbc::ir::LabelTarget t = r.unwrap();
            if (t.isEnd()) return false;
            for (ca::usize i = 0; i < order.size(); ++i) {
                if (order[i] == t.insn) {
                    out = i;
                    return true;
                }
            }
            return false;
        };

        while (!work.empty()) {
            const ca::usize pos = work.back();
            work.pop_back();
            if (pos == anchor) continue;   // 锚点只记录、不展开

            const Insn* in = unit_.dom.peek(order[pos]);
            if (in == nullptr) return false;
            ca::i32 delta = 0;
            if (!stackDelta(*lc_.session, *in, delta)) return false;
            const ca::i32 next = depth[pos] + delta;
            if (next < 0) return false;

            auto relax = [&](ca::usize p) -> bool {
                if (p < start || p > anchor) return false;   // 区间不收敛
                if (depth[p] < 0) {
                    depth[p] = next;
                    work.push_back(p);
                    return true;
                }
                return depth[p] == next;
            };

            switch (in->op) {
            case Op::GOTO: case Op::GOTO_W: case Op::JSR: case Op::JSR_W: {
                ca::usize t = 0;
                if (!targetPos(in->target, t) || !relax(t)) return false;
                break;
            }
            case Op::IFEQ: case Op::IFNE: case Op::IFLT: case Op::IFGE:
            case Op::IFGT: case Op::IFLE: case Op::IF_ICMPEQ: case Op::IF_ICMPNE:
            case Op::IF_ICMPLT: case Op::IF_ICMPGE: case Op::IF_ICMPGT: case Op::IF_ICMPLE:
            case Op::IF_ACMPEQ: case Op::IF_ACMPNE: case Op::IFNULL: case Op::IFNONNULL: {
                ca::usize t = 0;
                if (!targetPos(in->target, t) || !relax(t)) return false;
                if (!relax(pos + 1)) return false;
                break;
            }
            case Op::TABLESWITCH: case Op::LOOKUPSWITCH: {
                ca::usize t = 0;
                if (!targetPos(in->target, t) || !relax(t)) return false;
                for (const jbc::ir::LabelId lbl : in->targets) {
                    if (!targetPos(lbl, t) || !relax(t)) return false;
                }
                break;
            }
            case Op::IRETURN: case Op::LRETURN: case Op::FRETURN:
            case Op::DRETURN: case Op::ARETURN: case Op::RETURN:
            case Op::ATHROW: case Op::RET:
                break;   // 无后继
            default:
                if (!relax(pos + 1)) return false;
                break;
            }
        }

        if (depth[anchor] < 0) return false;   // 锚点不可达
        depthOut = depth[anchor];
        return true;
    }

    // ==== SCAN（方法内）=======================================================
    // isAnchor / noteCallee / structurallyOk / containsStringLdc / localsNotEscaping
    // / evaluate / anchorDepthUniform —— 全部纯读。
    bool tryFoldAt(ca::usize anchor)
    {
        const ca::usize lo = anchor > kRegionWindow ? anchor - kRegionWindow : 0;
        for (ca::usize start = lo; start < anchor; ++start) {
            if (!structurallyOk(start, anchor)) continue;
            if (!containsStringLdc(start, anchor)) continue;
            if (traceOn()) {
                std::fprintf(stderr, "[dasho.trace] candidate @start=%zu anchor=%zu\n",
                             static_cast<size_t>(start), static_cast<size_t>(anchor));
            }

            // 原地改写不擦除任何指令，因此「区间内写过的局部变量不得外泄」
            // 不是它的约束；整段折叠才需要。
            const bool fold = unit_.exc.empty();
            if (fold && !localsNotEscaping(start, anchor)) continue;

            std::u16string    plain;
            std::vector<bool> executed;
            if (!evaluate(start, anchor, plain, executed)) {
                DASHO_TRACE_REJECT("区间求值失败或多 seed 不一致");
                continue;
            }
            if (!plainUtf16Ok(plain)) {
                DASHO_TRACE_REJECT("明文不合法");
                continue;
            }

            // 死支（未执行）只允许纯计算：整段折叠会把它们删掉，擦掉副作用
            // 就是改语义。原地改写保留死支，无需此闸。
            if (fold) {
                bool deadSafe = true;
                const auto& order = unit_.dom.order();
                for (ca::usize i = start; i <= anchor; ++i) {
                    if (i < executed.size() && executed[i]) continue;
                    const Insn* in = unit_.dom.peek(order[i]);
                    if (in == nullptr || !erasableOp(*in)) {
                        deadSafe = false;
                        break;
                    }
                }
                if (!deadSafe) continue;
            }

            if (fold) {
                if (!rewriteRegion(start, anchor, plain)) continue;
                structural_ = true;
                return true;
            }
            if (!rewriteInPlace(start, anchor, plain, executed)) continue;
            return true;
        }
        return false;
    }
    // ==== REWRITE ==============================================================
    // 只消费 tryFoldAt 已经确认的区间与求值结果，不回头重新找站点。
    //
    //   rewriteRegion   无异常表 → 整段折叠成一条 ldc（指令数变 → 帧必须重算）
    //   rewriteInPlace  有异常表 → 原地替换，指令数与栈形不变（原帧逐条仍有效）
    //
    // 两条策略按"摘完帧还能不能重算"这个**语义事实**选择，与版本无关。
    //
    /// 整段折叠：区间内除头指令外全部擦除，头指令改写为 `ldc 明文`。
    /// 前置条件由 tryFoldAt 保证（无异常表、死支可擦、局部不外泄）。
    bool rewriteRegion(ca::usize start, ca::usize end, const std::u16string& plain)
    {
        std::string mutf8;
        if (!utf16ToMutf8(plain, mutf8)) return false;

        auto&             dom   = unit_.dom;
        const auto&       order = dom.order();
        const InsnId      head  = order[start];

        // 区间内部的边界标签（调试范围等）统一迁到新头指令，否则批量删除会
        // 因「存活 Boundary 标签锚定」被拒。
        for (ca::usize i = start + 1; i <= end; ++i) {
            for (const jbc::ir::LabelId lbl : dom.labelsAt(order[i])) {
                if (dom.bindLabel(lbl, head).is_err()) return false;
            }
        }

        std::vector<InsnId> victims;
        victims.reserve(end - start);
        for (ca::usize i = start + 1; i <= end; ++i) victims.push_back(order[i]);
        if (dom.eraseBatch(victims).is_err()) return false;

        auto first = dom.get(head);
        if (first.is_err()) return false;
        Insn* in  = first.unwrap();
        in->op    = Op::LDC;
        in->a     = 0;
        in->b     = 0;
        in->cp    = 0;
        in->wide  = false;
        in->target = jbc::ir::kNoLabel;
        in->targets.clear();
        in->keys.clear();
        return lc_.session->setString(*in, mutf8);
    }

    /// 原地改写：只把密文 `ldc` 换成明文，把解密调用换成等价的收栈序列。
    ///
    /// 为什么不做整段折叠：带异常表的方法里，既有 StackMapTable 含 verifier
    /// 要求的**保守 handler 帧**（try 区间内每条指令的 locals 全量合并），
    /// libjbc 的 Strict 域只从 canThrow 指令发异常边，重算结果比 verifier
    /// 精确，写回即 VerifyError。原地改写不改栈形，原帧逐条仍然成立。
    ///
    /// 收栈序列由**实参栈**推出，不靠猜签名：
    ///   `(String,int)`   → `pop`            （密文在下方，弹掉密钥）
    ///   `(String,int,int)` → `pop;pop`
    ///   `(int,String)`   → `swap;pop`       （密文在顶，掏掉下方的密钥）
    ///   `(String,String)`→ `swap;pop`
    bool rewriteInPlace(ca::usize start, ca::usize anchor, const std::u16string& plain,
                        const std::vector<bool>& executed)
    {
        std::string mutf8;
        if (!utf16ToMutf8(plain, mutf8)) return false;

        auto&       dom      = unit_.dom;
        const auto& order    = dom.order();
        const InsnId anchorId = order[anchor];

        const Insn* anchorInsn = dom.peek(anchorId);
        if (anchorInsn == nullptr) return false;
        ClassSession::MemberSem sem;
        if (!lc_.session->memberOf(*anchorInsn, sem)) {
            DASHO_TRACE_REJECT("锚点无成员引用");
            return false;
        }
        std::vector<bool> paramIsString;
        if (!parseParams(sem.desc, paramIsString)) {
            DASHO_TRACE_REJECT("描述符参数解析失败");
            return false;
        }

        // 实参栈：区间 [start, anchor) 的净效应是「压入实参」，因为整段
        // [start, anchor] 的净效应已被 evaluate 限定为恰好一个 String。
        std::vector<Value> args;
        {
            Interp interp(set_.map(),
                          [this](ClassSession& s, ca::usize i) { return set_.isPure(s, i); },
                          kSeeds[0]);
            // 区间 [start, anchor) 的净效应是「压入实参」（整段 [start, anchor]
            // 的净效应已被 evaluate 限定为恰好一个 String）。出口锚点本身不算
            // 越界：`ifeq/ifne` 模板的活动支常以 `goto 锚点` 收尾。
            auto r = interp.runRegion(*lc_.session, unit_, start, anchor - 1, nullptr, anchor);
            if (r.is_err()) {
                DASHO_TRACE_REJECT("实参区间求值失败");
                return false;
            }
            args = std::move(r).unwrap();
        }
        if (args.size() != paramIsString.size()) {
            DASHO_TRACE_REJECT("实参个数与描述符不符");
            return false;
        }
        for (const Value& v : args) {
            if (v.cat2()) {   // swap 对 cat-2 无定义
                DASHO_TRACE_REJECT("实参含 cat-2");
                return false;
            }
        }

        // 密文常量 = 区间内最后一条**被执行**的 String ldc（DashO 与自定义
        // 加密都把密文放在实参的最外侧，故它是最后一个被压入的字符串）。
        ca::usize      cipherPos = kNoPos;
        std::u16string cipherText;
        for (ca::usize i = start; i < anchor; ++i) {
            if (i < executed.size() && !executed[i]) continue;
            const Insn* in = dom.peek(order[i]);
            if (in == nullptr) continue;
            if (in->op != Op::LDC && in->op != Op::LDC_W) continue;
            ClassSession::ConstSem cs;
            if (!lc_.session->constOf(*in, cs)) continue;
            if (cs.kind != ClassSession::ConstSem::Kind::String) continue;
            std::u16string text;
            if (!mutf8ToUtf16(cs.str, text)) continue;
            cipherPos  = i;
            cipherText = std::move(text);
        }
        if (cipherPos == kNoPos) {
            DASHO_TRACE_REJECT("区间内无已执行的 String ldc");
            return false;
        }

        // 双信号交叉校验：实参栈里文本等于该常量的位置必须唯一，且描述符
        // 在同一位置声明为 String。字节码来源与运行期栈两个独立信号一致，
        // 才认定「这一位就是密文」——不一致说明不是可安全改写的形态。
        ca::usize pos  = kNoPos;
        ca::usize hits = 0;
        for (ca::usize k = 0; k < args.size(); ++k) {
            const std::u16string* t = strOfValue(args[k]);
            if (t == nullptr || *t != cipherText) continue;
            pos = k;
            ++hits;
        }
        if (hits != 1 || pos == kNoPos) {
            DASHO_TRACE_REJECT("密文常量在实参栈中不唯一");
            return false;
        }
        if (!paramIsString[pos]) {
            DASHO_TRACE_REJECT("密文位置在描述符中不是 String");
            return false;
        }

        // 栈深一致性：所有路径到锚点的槽数必须相同且等于实参槽数——收栈
        // 序列按「实参槽数」生成，非对称路径会让它在某条路径上欠栈。
        ca::i32 depth = 0;
        if (!anchorDepthUniform(start, anchor, depth)) {
            DASHO_TRACE_REJECT("锚点栈深不一致");
            return false;
        }
        if (depth != static_cast<ca::i32>(args.size())) {
            DASHO_TRACE_REJECT("锚点栈深与实参槽数不符");
            return false;
        }

        // 收栈序列：上方实参逐个 pop（cat-1 已由上面保证）；下方实参逐个
        // swap;pop —— 每轮把紧贴栈顶下方的那一个挤掉，密文始终留在栈顶。
        std::vector<Op> seq;
        for (ca::usize k = args.size(); k-- > pos + 1;) seq.push_back(Op::POP);
        for (ca::usize k = pos; k-- > 0;) {
            seq.push_back(Op::SWAP);
            seq.push_back(Op::POP);
        }
        if (seq.empty()) seq.push_back(Op::NOP);   // (String)String：调用本身就是恒等

        // 先插后改：插入是唯一可能失败的步骤，失败可原样撤回；随后的
        // setString 与锚点改写都不会失败，不存在半改状态。
        const InsnId cipherId = order[cipherPos];
        std::vector<InsnId> added;
        InsnId              tail = anchorId;
        for (ca::usize k = 1; k < seq.size(); ++k) {
            Insn add;
            add.op = seq[k];
            auto ins = dom.insertAfter(tail, std::move(add));
            if (ins.is_err()) {
                for (const InsnId id : added) (void)dom.erase(id);
                DASHO_TRACE_REJECT("插入收栈指令失败");
                return false;
            }
            tail = ins.unwrap();
            added.push_back(tail);
        }

        auto cipherInsn = dom.get(cipherId);
        if (cipherInsn.is_err() || !lc_.session->setString(*cipherInsn.unwrap(), mutf8)) {
            for (const InsnId id : added) (void)dom.erase(id);
            DASHO_TRACE_REJECT("密文常量改写失败");
            return false;
        }

        auto anchorPtr = dom.get(anchorId);
        if (anchorPtr.is_err()) return false;
        Insn* a   = anchorPtr.unwrap();
        a->op     = seq[0];
        a->a      = 0;
        a->b      = 0;
        a->cp     = 0;
        a->wide   = false;
        a->target = jbc::ir::kNoLabel;
        a->targets.clear();
        a->keys.clear();
        return true;
    }

    ClassSet&   set_;
    LoadedClass& lc_;
    CodeUnit&   unit_;

    /// 本方法的改写是否增删了指令（决定写回时是否需要再生帧）。
    /// 只有整段折叠会置真；原地改写的插入保持「帧目标 ↔ 指令句柄」绑定，
    /// 由 lower 的 relocation 精确搬运，不需要（也不能）重算帧。
    bool structural_ = false;

    std::unordered_set<std::string> callees_;
};

}   // namespace

// ==== PROOF ===================================================================
// 判定 + 回滚。
//
//   类级门  set.commit —— 帧再生 + 校验器 + 类数不增，失败回滚该类
//   死类门  只删"零引用 + 全部方法折叠过 + 自己就是解密器形态"的类
//   包级门  整包不发布
//
// 帧是这里的核心：整段折叠改了指令数 → 既有帧目标悬垂 → 交给 commit 按新布局
// 再生；原地改写不动指令数 → 原帧（含 verifier 要求的保守 handler 帧）逐条有效。
// 两条策略的差别全在"能不能安全地重算帧"。
mjbase::spi::PreservedAnalyses DashoStringDecryptPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    ClassSet set;
    set.load(container);
    if (set.classes().empty()) return mjbase::spi::PreservedAnalyses::all();

    ca::usize                       folded = 0;
    ca::usize                       dirtyClasses = 0;
    std::unordered_set<std::string> decryptors;
    for (LoadedClass& lc : set.classes()) {
        const ca::usize n = lc.session->unit().methods.size();
        for (ca::usize i = 0; i < n; ++i) {
            if (!jbc::ir::hasCode(*lc.session, i)) continue;
            auto promoted = jbc::ir::promoteMethod(*lc.session, i);
            if (promoted.is_err()) continue;
            jbc::ir::CodeUnitRef unit = std::move(promoted).unwrap();
            MethodFolder         folder(set, lc, *unit);
            const ca::usize      k = folder.run();
            if (k > 0) {
                lc.dirty.push_back(DirtyMethod{i, folder.structural()});
                folded += k;
                decryptors.insert(folder.callees().begin(), folder.callees().end());
            }
        }
        if (!lc.dirty.empty()) ++dirtyClasses;
    }

    if (folded == 0) {
        CA_LOG_INFO("[dasho.StringDecrypt] 未发现可折叠的加密调用点");
        return mjbase::spi::PreservedAnalyses::all();
    }

    const ca::usize written = set.commit(container);

    // ── 清扫死解密器类 ──
    // 调用点全部折叠后，解密器类（DashO 生成的 `Owner$1`）通常再无引用。
    // 只在"全部写回成功"时删：任一类写回失败就会留在原始字节上，而原始字节
    // 里调用点还在，删掉解密器就是拆掉活代码的依赖。
    DeadClassSweep sweep;
    if (written == dirtyClasses) {
        // 候选 = 锚点指向过的类 ∪ 形态上就是解密器的类。
        // 后者补上前者的漏网：DashO 的"双支"结构里可能把对另一个解密器的
        // 调用塞进死支，随整段折叠一起被擦掉——那个解密器从未当过锚点，
        // 但此刻同样已经没有调用者了（实测 `DataOfSquare$1` 即此情形）。
        std::unordered_set<std::string> candidates = collectDecryptorClasses(set);
        candidates.insert(decryptors.begin(), decryptors.end());
        sweep = eraseUnreferencedClasses(container, set, candidates);
    }

    CA_LOG_INFO("[dasho.StringDecrypt] 折叠 {} 个调用点，写回 {} 个类，"
                "删除 {} 个死解密器类（保留有引用 {} 个）",
                folded, written, sweep.deleted, sweep.keptReferenced);
    return mjbase::spi::PreservedAnalyses::none();
}

Result<void, std::string> registerStringDecrypt(mjbase::spi::PassNameRegistry& reg)
{
    if (Result<void, mjbase::spi::RegistryError> r = reg.registerGlobalPass(
            "dasho.StringDecrypt",
            [] { return std::make_unique<DashoStringDecryptPass>(); },
            "Fold DashO string-encryption call sites (key arithmetic + opaque predicate"
            " + decoy branch + decryptor chain) into plaintext ldc by interpreting the"
            " whole region; no name/signature assumptions.");
        r.is_err()) {
        return Err(std::string("dasho.StringDecrypt"));
    }
    return Ok();
}

}   // namespace dasho
