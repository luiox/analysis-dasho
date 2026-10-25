#include "dasho/jvm_interp.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "jbc/core/opcode.hpp"
#include "jstr/conversion.hpp"
#include "jstr/mutf8_string.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

namespace dasho {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;

// ── 值构造 ──

Value Value::ofInt(ca::i32 v)
{
    Value o;
    o.kind = Kind::Int;
    o.i    = v;
    return o;
}

Value Value::ofLong(ca::i64 v)
{
    Value o;
    o.kind = Kind::Long;
    o.j    = v;
    return o;
}

Value Value::ofStr(std::u16string v)
{
    Value o;
    o.kind = Kind::Str;
    o.s    = std::move(v);
    return o;
}

const char* to_string(InterpFail f) noexcept
{
    switch (f) {
    case InterpFail::Ok:             return "Ok";
    case InterpFail::Unsupported:    return "Unsupported";
    case InterpFail::StackUnderflow: return "StackUnderflow";
    case InterpFail::BadOperand:     return "BadOperand";
    case InterpFail::Runtime:        return "Runtime";
    case InterpFail::StepLimit:      return "StepLimit";
    case InterpFail::OutOfRegion:    return "OutOfRegion";
    case InterpFail::NotConcrete:    return "NotConcrete";
    }
    return "?";
}

namespace {

// ── 字符串编解码（与 allatori pass 同路径：MUTF-8 ↔ UTF-16 码元）──
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
const std::u16string* strOf(const Value& v)
{
    if (v.kind == Value::Kind::Str) return &v.s;
    if (v.kind == Value::Kind::Obj && v.obj && v.obj->kind == HeapObj::Kind::StringVal) {
        return &v.obj->text;
    }
    return nullptr;
}

/// 引用值 → StringBuilder 缓冲（append/insert 的接收者）。
std::u16string* sbOf(const Value& v)
{
    if (v.kind == Value::Kind::Obj && v.obj && v.obj->kind == HeapObj::Kind::StringBuilder) {
        return &v.obj->text;
    }
    return nullptr;
}

/// 十进制文本（String.valueOf(int) / StringBuilder.append(int) 语义）。
std::u16string decOf(ca::i64 v)
{
    std::u16string out;
    if (v == 0) {
        out.push_back(u'0');
        return out;
    }
    const bool neg = v < 0;
    ca::u64    mag = neg ? static_cast<ca::u64>(-(v + 1)) + 1 : static_cast<ca::u64>(v);
    while (mag != 0) {
        out.push_back(static_cast<char16_t>(u'0' + (mag % 10)));
        mag /= 10;
    }
    if (neg) out.push_back(u'-');
    std::reverse(out.begin(), out.end());
    return out;
}

/// 描述符参数类型序列（'I' 'J' 'F' 'D' 'L' '['），失败返回 false。
bool paramTypes(std::string_view desc, std::vector<char>& out)
{
    out.clear();
    if (desc.empty() || desc.front() != '(') return false;
    ca::usize i = 1;
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        if (c == 'L') {
            const auto semi = desc.find(';', i);
            if (semi == std::string_view::npos) return false;
            out.push_back('L');
            i = semi + 1;
        }
        else if (c == '[') {
            ca::usize j = i;
            while (j < desc.size() && desc[j] == '[') ++j;
            if (j >= desc.size()) return false;
            if (desc[j] == 'L') {
                const auto semi = desc.find(';', j);
                if (semi == std::string_view::npos) return false;
                i = semi + 1;
            }
            else {
                i = j + 1;
            }
            out.push_back('[');
        }
        else if (c == 'J' || c == 'D' || c == 'F' || c == 'I' || c == 'Z' || c == 'B' ||
                 c == 'C' || c == 'S')
        {
            out.push_back(c);
            ++i;
        }
        else {
            return false;
        }
    }
    return i < desc.size() && desc[i] == ')';
}

char retType(std::string_view desc)
{
    const auto close = desc.find(')');
    if (close == std::string_view::npos || close + 1 >= desc.size()) return 'V';
    return desc[close + 1];
}

// ── java.util.Random（LCG 状态，种子由调用方给，用于多 seed 一致性判定）──

constexpr ca::u64 kRandomMask = (1ULL << 48) - 1;

ca::i32 randomNext(ca::u64& seed, int bits)
{
    seed = (seed * 0x5DEECE66DULL + 0xBULL) & kRandomMask;
    return static_cast<ca::i32>(seed >> (48 - bits));
}

bool randomNextInt(ca::u64& seed, ca::i32 bound, ca::i32& out)
{
    if (bound <= 0) return false;
    if ((bound & -bound) == bound) {
        out = static_cast<ca::i32>((static_cast<ca::i64>(bound) * randomNext(seed, 31)) >> 31);
        return true;
    }
    for (int guard = 0; guard < 64; ++guard) {
        const ca::i32 bits = randomNext(seed, 31);
        const ca::i32 val  = bits % bound;
        if (bits - val + (bound - 1) >= 0) {
            out = val;
            return true;
        }
    }
    return false;
}

}   // namespace

bool isIntrinsicCall(std::string_view owner, std::string_view name, std::string_view desc)
{
    if (owner == "java/lang/Object") return name == "<init>";
    if (owner == "java/lang/String") {
        if (name == "length") return desc == "()I";
        if (name == "charAt") return desc == "(I)C";
        if (name == "toCharArray") return desc == "()[C";
        if (name == "intern") return desc == "()Ljava/lang/String;";
        if (name == "valueOf") {
            return desc == "([C)Ljava/lang/String;" || desc == "([CII)Ljava/lang/String;" ||
                   desc == "(C)Ljava/lang/String;" || desc == "(I)Ljava/lang/String;" ||
                   desc == "(J)Ljava/lang/String;";
        }
        if (name == "concat") return desc == "(Ljava/lang/String;)Ljava/lang/String;";
        if (name == "substring") {
            return desc == "(I)Ljava/lang/String;" || desc == "(II)Ljava/lang/String;";
        }
        if (name == "<init>") {
            return desc == "()V" || desc == "([C)V" || desc == "([CII)V" ||
                   desc == "(Ljava/lang/String;)V";
        }
        return false;
    }
    if (owner == "java/lang/StringBuilder" || owner == "java/lang/StringBuffer") {
        if (name == "<init>") return desc == "()V" || desc == "(I)V" ||
                                      desc == "(Ljava/lang/String;)V";
        if (name == "append") {
            return desc == "(Ljava/lang/String;)Ljava/lang/StringBuilder;" ||
                   desc == "(Ljava/lang/String;)Ljava/lang/StringBuffer;" ||
                   desc == "(C)Ljava/lang/StringBuilder;" ||
                   desc == "(C)Ljava/lang/StringBuffer;" ||
                   desc == "(I)Ljava/lang/StringBuilder;" ||
                   desc == "(I)Ljava/lang/StringBuffer;" ||
                   desc == "(J)Ljava/lang/StringBuilder;" ||
                   desc == "(J)Ljava/lang/StringBuffer;" ||
                   desc == "([C)Ljava/lang/StringBuilder;" ||
                   desc == "([C)Ljava/lang/StringBuffer;";
        }
        if (name == "insert") {
            return desc == "(ILjava/lang/String;)Ljava/lang/StringBuilder;" ||
                   desc == "(ILjava/lang/String;)Ljava/lang/StringBuffer;";
        }
        if (name == "toString") return desc == "()Ljava/lang/String;";
        if (name == "length") return desc == "()I";
        if (name == "charAt") return desc == "(I)C";
        return false;
    }
    if (owner == "java/util/Random") {
        if (name == "<init>") return desc == "()V" || desc == "(J)V";
        if (name == "nextInt") return desc == "()I" || desc == "(I)I";
        if (name == "nextLong") return desc == "()J";
        if (name == "nextBoolean") return desc == "()Z";
        return false;
    }
    if (owner == "java/lang/Math") {
        if (name == "abs") return desc == "(I)I" || desc == "(J)J";
        if (name == "min" || name == "max") return desc == "(II)I";
        if (name == "negateExact") return desc == "(I)I";
        return false;
    }
    if (owner == "java/lang/Integer") {
        if (name == "parseInt") return desc == "(Ljava/lang/String;)I";
        if (name == "toString") return desc == "(I)Ljava/lang/String;";
        return false;
    }
    if (owner == "java/lang/Character" && name == "toString") {
        return desc == "(C)Ljava/lang/String;";
    }
    if (owner == "java/lang/System" && name == "arraycopy") {
        return desc == "(Ljava/lang/Object;ILjava/lang/Object;II)V";
    }
    return false;
}

// ── 解释器 ──

/// 「无此下标」哨兵（order 下标域）。
constexpr ca::usize kNoPos = static_cast<ca::usize>(-1);

struct Interp::Frame
{
    ClassSession*        session = nullptr;
    CodeUnit*            unit    = nullptr;
    const std::vector<InsnId>* order = nullptr;
    std::unordered_map<ca::u32, ca::usize> indexOf;

    std::unordered_map<ca::i32, Value> locals;
    std::vector<Value>                 stack;
    ca::usize                          pc = 0;
    int                                steps = 0;
    int                                depth = 0;

    bool              regionMode = false;
    ca::usize         regionStart = 0;
    ca::usize         regionEnd   = 0;
    /// 区间模式下的「合法出口」：控制流转移到这一条指令即视为区间正常结束
    /// （该指令本身不执行）。用于「求区间内实参栈」——最后一条分支往往以
    /// `goto 锚点` 收尾，锚点恰在区间外，严格越界判定会把这种形态判失败。
    ca::usize         exitOk      = kNoPos;
    std::vector<bool>* executed   = nullptr;

    Value pop()
    {
        if (stack.empty()) return Value{};   // 由调用点先行检查
        Value v = std::move(stack.back());
        stack.pop_back();
        return v;
    }
};

Interp::Interp(const ClassMap& classes, PureQuery pure, ca::u64 seedBase)
    : classes_(classes), pure_(std::move(pure)), seedBase_(seedBase)
{}

namespace {

/// 局部变量下标解码（_0.._3 缩略形态与显式索引）。
ca::i32 localIndex(const Insn& in)
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

bool isLoad(const Op op)
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

bool isStore(const Op op)
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

bool isIntBinOp(const Op op)
{
    switch (op) {
    case Op::IADD: case Op::ISUB: case Op::IMUL: case Op::IDIV: case Op::IREM:
    case Op::ISHL: case Op::ISHR: case Op::IUSHR: case Op::IAND: case Op::IOR: case Op::IXOR:
        return true;
    default:
        return false;
    }
}

bool isLongBinOp(const Op op)
{
    switch (op) {
    case Op::LADD: case Op::LSUB: case Op::LMUL: case Op::LDIV: case Op::LREM:
    case Op::LSHL: case Op::LSHR: case Op::LUSHR: case Op::LAND: case Op::LOR: case Op::LXOR:
        return true;
    default:
        return false;
    }
}

}   // namespace

Result<void, InterpFail> Interp::run(Frame& f, bool regionMode, ca::usize regionEnd)
{
    const CodeUnit& unit  = *f.unit;
    const auto&     order = *f.order;

    auto resolveTarget = [&](jbc::ir::LabelId label, ca::usize& outIdx) -> bool {
        const auto resolved = unit.dom.resolveLabel(label);
        if (resolved.is_err()) return false;
        const jbc::ir::LabelTarget t = resolved.unwrap();
        if (t.isEnd()) {
            outIdx = order.size();
        }
        else {
            const auto it = f.indexOf.find(t.insn.slot);
            if (it == f.indexOf.end()) return false;
            outIdx = it->second;
        }
        if (regionMode && (outIdx < f.regionStart || outIdx > regionEnd) &&
            outIdx != f.exitOk)
        {
            return false;
        }
        return true;
    };

    while (f.pc < order.size()) {
        if (regionMode && f.pc == f.exitOk) return Ok();
        if (regionMode && f.pc > regionEnd) return Err(InterpFail::OutOfRegion);
        if (++f.steps > 200000) return Err(InterpFail::StepLimit);
        const InsnId        id  = order[f.pc];
        const Insn*         in  = unit.dom.peek(id);
        const ca::usize     cur = f.pc;
        if (in == nullptr) return Err(InterpFail::BadOperand);
        ++f.pc;
        if (f.executed != nullptr && cur < f.executed->size()) (*f.executed)[cur] = true;

        auto popChecked = [&](Value& v) -> bool {
            if (f.stack.empty()) return false;
            v = f.pop();
            return true;
        };
        auto popInt = [&](ca::i32& v) -> bool {
            Value x;
            if (!popChecked(x)) return false;
            if (x.kind == Value::Kind::Unknown) {
                v = 0;
                return true;   // Unknown 以 0 参与，结果由后续 Unknown 传播兜底
            }
            if (x.kind != Value::Kind::Int) return false;
            v = x.i;
            return true;
        };
        auto popLong = [&](ca::i64& v) -> bool {
            Value x;
            if (!popChecked(x)) return false;
            if (x.kind == Value::Kind::Unknown) {
                v = 0;
                return true;
            }
            if (x.kind != Value::Kind::Long) return false;
            v = x.j;
            return true;
        };

        switch (in->op) {
        case Op::NOP:
            break;

        // ---- 常量 ----
        case Op::ACONST_NULL: {
            Value v;
            v.kind = Value::Kind::Null;
            f.stack.push_back(std::move(v));
            break;
        }
        case Op::ICONST_M1: case Op::ICONST_0: case Op::ICONST_1: case Op::ICONST_2:
        case Op::ICONST_3: case Op::ICONST_4: case Op::ICONST_5: {
            ca::i32 v = 0;
            if (in->op == Op::ICONST_M1) v = -1;
            else v = static_cast<ca::i32>(in->op) - static_cast<ca::i32>(Op::ICONST_0);
            f.stack.push_back(Value::ofInt(v));
            break;
        }
        case Op::LCONST_0: case Op::LCONST_1:
            f.stack.push_back(Value::ofLong(in->op == Op::LCONST_1 ? 1 : 0));
            break;
        case Op::FCONST_0: case Op::FCONST_1: case Op::FCONST_2: {
            Value v;
            v.kind = Value::Kind::Float;
            f.stack.push_back(std::move(v));
            break;
        }
        case Op::DCONST_0: case Op::DCONST_1: {
            Value v;
            v.kind = Value::Kind::Double;
            f.stack.push_back(std::move(v));
            break;
        }
        case Op::BIPUSH: case Op::SIPUSH:
            f.stack.push_back(Value::ofInt(in->a));
            break;
        case Op::LDC: case Op::LDC_W: case Op::LDC2_W: {
            ClassSession::ConstSem cs;
            if (!f.session->constOf(*in, cs)) return Err(InterpFail::BadOperand);
            switch (cs.kind) {
            case ClassSession::ConstSem::Kind::Int:
                f.stack.push_back(Value::ofInt(static_cast<ca::i32>(cs.num)));
                break;
            case ClassSession::ConstSem::Kind::Long:
                f.stack.push_back(Value::ofLong(cs.num));
                break;
            case ClassSession::ConstSem::Kind::Float: {
                Value v;
                v.kind = Value::Kind::Float;
                f.stack.push_back(std::move(v));
                break;
            }
            case ClassSession::ConstSem::Kind::Double: {
                Value v;
                v.kind = Value::Kind::Double;
                f.stack.push_back(std::move(v));
                break;
            }
            case ClassSession::ConstSem::Kind::String: {
                std::u16string text;
                if (!mutf8ToUtf16(cs.str, text)) return Err(InterpFail::BadOperand);
                f.stack.push_back(Value::ofStr(std::move(text)));
                break;
            }
            default:
                return Err(InterpFail::Unsupported);
            }
            break;
        }

        // ---- 局部变量 ----
        default:
            if (isLoad(in->op)) {
                const ca::i32 idx = localIndex(*in);
                const auto    it  = f.locals.find(idx);
                if (it == f.locals.end()) {
                    f.stack.push_back(Value{});   // 未播种 → Unknown
                }
                else {
                    f.stack.push_back(it->second);
                }
                break;
            }
            if (isStore(in->op)) {
                Value v;
                if (!popChecked(v)) return Err(InterpFail::StackUnderflow);
                f.locals[localIndex(*in)] = std::move(v);
                break;
            }
            if (isIntBinOp(in->op)) {
                Value bv;
                Value av;
                if (!popChecked(bv) || !popChecked(av)) return Err(InterpFail::StackUnderflow);
                if (av.kind == Value::Kind::Unknown || bv.kind == Value::Kind::Unknown) {
                    f.stack.push_back(Value{});
                    break;
                }
                if (av.kind != Value::Kind::Int || bv.kind != Value::Kind::Int) {
                    return Err(InterpFail::BadOperand);
                }
                const ca::i32 a = av.i;
                const ca::i32 b = bv.i;
                ca::i32       r = 0;
                switch (in->op) {
                case Op::IADD: r = a + b; break;
                case Op::ISUB: r = a - b; break;
                case Op::IMUL: r = a * b; break;
                case Op::IDIV:
                    if (b == 0) return Err(InterpFail::Runtime);
                    r = (a == std::numeric_limits<ca::i32>::min() && b == -1) ? a : a / b;
                    break;
                case Op::IREM:
                    if (b == 0) return Err(InterpFail::Runtime);
                    r = (a == std::numeric_limits<ca::i32>::min() && b == -1) ? 0 : a % b;
                    break;
                case Op::ISHL: r = a << (b & 31); break;
                case Op::ISHR: r = a >> (b & 31); break;
                case Op::IUSHR: r = static_cast<ca::i32>(static_cast<ca::u32>(a) >> (b & 31)); break;
                case Op::IAND: r = a & b; break;
                case Op::IOR:  r = a | b; break;
                case Op::IXOR: r = a ^ b; break;
                default: return Err(InterpFail::Unsupported);
                }
                f.stack.push_back(Value::ofInt(r));
                break;
            }
            if (isLongBinOp(in->op)) {
                Value bv;
                Value av;
                if (!popChecked(bv) || !popChecked(av)) return Err(InterpFail::StackUnderflow);
                if (av.kind == Value::Kind::Unknown || bv.kind == Value::Kind::Unknown) {
                    f.stack.push_back(Value{});
                    break;
                }
                if (av.kind != Value::Kind::Long || bv.kind != Value::Kind::Long) {
                    return Err(InterpFail::BadOperand);
                }
                const ca::i64 a = av.j;
                const ca::i64 b = bv.j;
                ca::i64       r = 0;
                switch (in->op) {
                case Op::LADD: r = a + b; break;
                case Op::LSUB: r = a - b; break;
                case Op::LMUL: r = a * b; break;
                case Op::LDIV:
                    if (b == 0) return Err(InterpFail::Runtime);
                    r = a / b;
                    break;
                case Op::LREM:
                    if (b == 0) return Err(InterpFail::Runtime);
                    r = a % b;
                    break;
                case Op::LSHL: r = a << (b & 63); break;
                case Op::LSHR: r = a >> (b & 63); break;
                case Op::LUSHR:
                    r = static_cast<ca::i64>(static_cast<ca::u64>(a) >> (b & 63));
                    break;
                case Op::LAND: r = a & b; break;
                case Op::LOR:  r = a | b; break;
                case Op::LXOR: r = a ^ b; break;
                default: return Err(InterpFail::Unsupported);
                }
                f.stack.push_back(Value::ofLong(r));
                break;
            }

            switch (in->op) {
            case Op::INEG: {
                ca::i32 a = 0;
                if (!popInt(a)) return Err(InterpFail::StackUnderflow);
                f.stack.push_back(Value::ofInt(-a));
                break;
            }
            case Op::LNEG: {
                ca::i64 a = 0;
                if (!popLong(a)) return Err(InterpFail::StackUnderflow);
                f.stack.push_back(Value::ofLong(-a));
                break;
            }
            case Op::IINC: {
                const auto it = f.locals.find(in->a);
                if (it == f.locals.end()) {
                    Value v;
                    v.kind = Value::Kind::Unknown;
                    f.locals[in->a] = std::move(v);
                }
                else if (it->second.kind == Value::Kind::Int) {
                    it->second.i += in->b;
                }
                else if (it->second.kind != Value::Kind::Unknown) {
                    return Err(InterpFail::BadOperand);
                }
                break;
            }
            case Op::I2L:
            case Op::I2F:
            case Op::I2D:
            case Op::L2I:
            case Op::L2F:
            case Op::L2D:
            case Op::F2I:
            case Op::F2L:
            case Op::F2D:
            case Op::D2I:
            case Op::D2L:
            case Op::D2F: {
                Value v;
                if (!popChecked(v)) return Err(InterpFail::StackUnderflow);
                Value out;
                switch (in->op) {
                case Op::I2L: out = Value::ofLong(v.kind == Value::Kind::Int ? v.i : 0); break;
                case Op::L2I: out = Value::ofInt(v.kind == Value::Kind::Long ? static_cast<ca::i32>(v.j) : 0); break;
                case Op::I2F: case Op::L2F: case Op::D2F:
                    out.kind = Value::Kind::Float;
                    break;
                case Op::I2D: case Op::L2D: case Op::F2D:
                    out.kind = Value::Kind::Double;
                    break;
                default:
                    out.kind = Value::Kind::Int;
                    break;
                }
                f.stack.push_back(std::move(out));
                break;
            }
            case Op::I2B: case Op::I2C: case Op::I2S: {
                ca::i32 a = 0;
                if (!popInt(a)) return Err(InterpFail::StackUnderflow);
                ca::i32 r = a;
                if (in->op == Op::I2B) r = static_cast<ca::i8>(a);
                else if (in->op == Op::I2C) r = static_cast<ca::u16>(a);
                else r = static_cast<ca::i16>(a);
                f.stack.push_back(Value::ofInt(r));
                break;
            }

            // ---- 栈操作 ----
            case Op::POP: {
                Value v;
                if (!popChecked(v)) return Err(InterpFail::StackUnderflow);
                if (v.cat2()) return Err(InterpFail::BadOperand);
                break;
            }
            case Op::POP2: {
                Value v;
                if (!popChecked(v)) return Err(InterpFail::StackUnderflow);
                if (!v.cat2()) {
                    Value w;
                    if (!popChecked(w)) return Err(InterpFail::StackUnderflow);
                    if (w.cat2()) return Err(InterpFail::BadOperand);
                }
                break;
            }
            case Op::DUP: {
                if (f.stack.empty()) return Err(InterpFail::StackUnderflow);
                if (f.stack.back().cat2()) return Err(InterpFail::BadOperand);
                f.stack.push_back(f.stack.back());
                break;
            }
            case Op::DUP_X1: {
                if (f.stack.size() < 2) return Err(InterpFail::StackUnderflow);
                const Value v1 = f.stack.back();
                const Value v2 = f.stack[f.stack.size() - 2];
                if (v1.cat2() || v2.cat2()) return Err(InterpFail::BadOperand);
                f.stack.pop_back();
                f.stack.pop_back();
                f.stack.push_back(v1);
                f.stack.push_back(v2);
                f.stack.push_back(v1);
                break;
            }
            case Op::DUP_X2: {
                if (f.stack.size() < 2) return Err(InterpFail::StackUnderflow);
                const Value v1 = f.stack.back();
                if (v1.cat2()) return Err(InterpFail::BadOperand);
                const Value v2 = f.stack[f.stack.size() - 2];
                if (v2.cat2()) {
                    f.stack.pop_back();
                    f.stack.pop_back();
                    f.stack.push_back(v1);
                    f.stack.push_back(v2);
                    f.stack.push_back(v1);
                    break;
                }
                if (f.stack.size() < 3) return Err(InterpFail::StackUnderflow);
                const Value v3 = f.stack[f.stack.size() - 3];
                if (v3.cat2()) return Err(InterpFail::BadOperand);
                f.stack.pop_back();
                f.stack.pop_back();
                f.stack.pop_back();
                f.stack.push_back(v1);
                f.stack.push_back(v3);
                f.stack.push_back(v2);
                f.stack.push_back(v1);
                break;
            }
            case Op::DUP2: {
                if (f.stack.empty()) return Err(InterpFail::StackUnderflow);
                const Value v1 = f.stack.back();
                if (v1.cat2()) {
                    f.stack.push_back(v1);
                    break;
                }
                if (f.stack.size() < 2) return Err(InterpFail::StackUnderflow);
                const Value v2 = f.stack[f.stack.size() - 2];
                if (v2.cat2()) return Err(InterpFail::BadOperand);
                f.stack.push_back(v2);
                f.stack.push_back(v1);
                break;
            }
            case Op::DUP2_X1: {
                if (f.stack.size() < 2) return Err(InterpFail::StackUnderflow);
                const Value v1 = f.stack.back();
                if (v1.cat2()) {
                    const Value v2 = f.stack[f.stack.size() - 2];
                    if (v2.cat2()) return Err(InterpFail::BadOperand);
                    f.stack.pop_back();
                    f.stack.pop_back();
                    f.stack.push_back(v1);
                    f.stack.push_back(v2);
                    f.stack.push_back(v1);
                    break;
                }
                if (f.stack.size() < 3) return Err(InterpFail::StackUnderflow);
                const Value v2 = f.stack[f.stack.size() - 2];
                const Value v3 = f.stack[f.stack.size() - 3];
                if (v2.cat2() || v3.cat2()) return Err(InterpFail::BadOperand);
                f.stack.pop_back();
                f.stack.pop_back();
                f.stack.pop_back();
                f.stack.push_back(v2);
                f.stack.push_back(v1);
                f.stack.push_back(v3);
                f.stack.push_back(v2);
                f.stack.push_back(v1);
                break;
            }
            case Op::DUP2_X2: {
                if (f.stack.size() < 2) return Err(InterpFail::StackUnderflow);
                const Value v1 = f.stack.back();
                const Value v2 = f.stack[f.stack.size() - 2];
                if (v1.cat2() && v2.cat2()) {
                    f.stack.pop_back();
                    f.stack.pop_back();
                    f.stack.push_back(v1);
                    f.stack.push_back(v2);
                    f.stack.push_back(v1);
                    break;
                }
                if (v1.cat2()) {
                    if (f.stack.size() < 3) return Err(InterpFail::StackUnderflow);
                    const Value v3 = f.stack[f.stack.size() - 3];
                    if (v3.cat2()) return Err(InterpFail::BadOperand);
                    f.stack.pop_back();
                    f.stack.pop_back();
                    f.stack.pop_back();
                    f.stack.push_back(v1);
                    f.stack.push_back(v3);
                    f.stack.push_back(v2);
                    f.stack.push_back(v1);
                    break;
                }
                if (v2.cat2()) return Err(InterpFail::BadOperand);
                if (f.stack.size() < 4) return Err(InterpFail::StackUnderflow);
                {
                    const Value v3 = f.stack[f.stack.size() - 3];
                    const Value v4 = f.stack[f.stack.size() - 4];
                    if (v3.cat2() || v4.cat2()) return Err(InterpFail::BadOperand);
                    f.stack.pop_back();
                    f.stack.pop_back();
                    f.stack.pop_back();
                    f.stack.pop_back();
                    f.stack.push_back(v2);
                    f.stack.push_back(v1);
                    f.stack.push_back(v4);
                    f.stack.push_back(v3);
                    f.stack.push_back(v2);
                    f.stack.push_back(v1);
                }
                break;
            }
            case Op::SWAP: {
                if (f.stack.size() < 2) return Err(InterpFail::StackUnderflow);
                std::swap(f.stack[f.stack.size() - 1], f.stack[f.stack.size() - 2]);
                break;
            }

            // ---- 数组 ----
            case Op::NEWARRAY: {
                ca::i32 n = 0;
                if (!popInt(n)) return Err(InterpFail::StackUnderflow);
                if (n < 0) return Err(InterpFail::Runtime);
                Value v;
                v.kind = Value::Kind::Chars;
                v.arr  = std::make_shared<std::vector<ca::i32>>(static_cast<ca::usize>(n), 0);
                f.stack.push_back(std::move(v));
                break;
            }
            case Op::ARRAYLENGTH: {
                Value v;
                if (!popChecked(v)) return Err(InterpFail::StackUnderflow);
                if (v.kind != Value::Kind::Chars || !v.arr) return Err(InterpFail::Unsupported);
                f.stack.push_back(Value::ofInt(static_cast<ca::i32>(v.arr->size())));
                break;
            }
            case Op::CALOAD: case Op::IALOAD: case Op::BALOAD: case Op::SALOAD: {
                ca::i32 idx = 0;
                Value    arr;
                if (!popInt(idx) || !popChecked(arr)) return Err(InterpFail::StackUnderflow);
                if (arr.kind != Value::Kind::Chars || !arr.arr) return Err(InterpFail::Unsupported);
                if (idx < 0 || static_cast<ca::usize>(idx) >= arr.arr->size()) {
                    return Err(InterpFail::Runtime);
                }
                ca::i32 v = (*arr.arr)[static_cast<ca::usize>(idx)];
                if (in->op == Op::CALOAD) v = static_cast<ca::u16>(v);
                else if (in->op == Op::BALOAD) v = static_cast<ca::i8>(v);
                else if (in->op == Op::SALOAD) v = static_cast<ca::i16>(v);
                f.stack.push_back(Value::ofInt(v));
                break;
            }
            case Op::CASTORE: case Op::IASTORE: case Op::BASTORE: case Op::SASTORE: {
                Value    val;
                ca::i32 idx = 0;
                Value    arr;
                if (!popChecked(val) || !popInt(idx) || !popChecked(arr)) {
                    return Err(InterpFail::StackUnderflow);
                }
                if (arr.kind != Value::Kind::Chars || !arr.arr) return Err(InterpFail::Unsupported);
                if (val.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
                if (idx < 0 || static_cast<ca::usize>(idx) >= arr.arr->size()) {
                    return Err(InterpFail::Runtime);
                }
                ca::i32 store = val.i;
                if (in->op == Op::CASTORE) store = static_cast<ca::u16>(val.i);
                else if (in->op == Op::BASTORE) store = static_cast<ca::i8>(val.i);
                else if (in->op == Op::SASTORE) store = static_cast<ca::i16>(val.i);
                (*arr.arr)[static_cast<ca::usize>(idx)] = store;
                break;
            }

            // ---- 对象 ----
            case Op::NEW: {
                const std::string_view cls = f.session->classOf(*in);
                Value v;
                v.kind = Value::Kind::Obj;
                v.obj  = std::make_shared<HeapObj>();
                if (cls == "java/lang/StringBuilder" || cls == "java/lang/StringBuffer") {
                    v.obj->kind = HeapObj::Kind::StringBuilder;
                }
                else if (cls == "java/util/Random") {
                    v.obj->kind = HeapObj::Kind::Random;
                    v.obj->seed = seedBase_;
                }
                else if (cls == "java/lang/String") {
                    v.obj->kind = HeapObj::Kind::StringInit;
                }
                else {
                    v.obj->kind = HeapObj::Kind::Plain;
                }
                f.stack.push_back(std::move(v));
                break;
            }
            case Op::CHECKCAST:
                break;
            case Op::INSTANCEOF: {
                Value v;
                if (!popChecked(v)) return Err(InterpFail::StackUnderflow);
                f.stack.push_back(Value::ofInt(0));
                break;
            }

            // ---- 分支 ----
            case Op::GOTO: case Op::GOTO_W: {
                ca::usize t = 0;
                if (!resolveTarget(in->target, t)) return Err(InterpFail::OutOfRegion);
                f.pc = t;
                break;
            }
            case Op::IFEQ: case Op::IFNE: case Op::IFLT: case Op::IFGE: case Op::IFGT:
            case Op::IFLE: {
                Value c;
                if (!popChecked(c)) return Err(InterpFail::StackUnderflow);
                if (c.kind == Value::Kind::Unknown) return Err(InterpFail::NotConcrete);
                if (c.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
                bool taken = false;
                switch (in->op) {
                case Op::IFEQ: taken = c.i == 0; break;
                case Op::IFNE: taken = c.i != 0; break;
                case Op::IFLT: taken = c.i < 0; break;
                case Op::IFGE: taken = c.i >= 0; break;
                case Op::IFGT: taken = c.i > 0; break;
                case Op::IFLE: taken = c.i <= 0; break;
                default: break;
                }
                if (taken) {
                    ca::usize t = 0;
                    if (!resolveTarget(in->target, t)) return Err(InterpFail::OutOfRegion);
                    f.pc = t;
                }
                break;
            }
            case Op::IF_ICMPEQ: case Op::IF_ICMPNE: case Op::IF_ICMPLT: case Op::IF_ICMPGE:
            case Op::IF_ICMPGT: case Op::IF_ICMPLE: {
                Value b;
                Value a;
                if (!popChecked(b) || !popChecked(a)) return Err(InterpFail::StackUnderflow);
                if (a.kind == Value::Kind::Unknown || b.kind == Value::Kind::Unknown) {
                    return Err(InterpFail::NotConcrete);
                }
                if (a.kind != Value::Kind::Int || b.kind != Value::Kind::Int) {
                    return Err(InterpFail::BadOperand);
                }
                bool taken = false;
                switch (in->op) {
                case Op::IF_ICMPEQ: taken = a.i == b.i; break;
                case Op::IF_ICMPNE: taken = a.i != b.i; break;
                case Op::IF_ICMPLT: taken = a.i < b.i; break;
                case Op::IF_ICMPGE: taken = a.i >= b.i; break;
                case Op::IF_ICMPGT: taken = a.i > b.i; break;
                case Op::IF_ICMPLE: taken = a.i <= b.i; break;
                default: break;
                }
                if (taken) {
                    ca::usize t = 0;
                    if (!resolveTarget(in->target, t)) return Err(InterpFail::OutOfRegion);
                    f.pc = t;
                }
                break;
            }
            case Op::IFNULL: case Op::IFNONNULL: {
                Value a;
                if (!popChecked(a)) return Err(InterpFail::StackUnderflow);
                if (a.kind == Value::Kind::Unknown) return Err(InterpFail::NotConcrete);
                const bool isNull = a.kind == Value::Kind::Null;
                const bool taken  = in->op == Op::IFNULL ? isNull : !isNull;
                if (taken) {
                    ca::usize t = 0;
                    if (!resolveTarget(in->target, t)) return Err(InterpFail::OutOfRegion);
                    f.pc = t;
                }
                break;
            }
            case Op::IF_ACMPEQ: case Op::IF_ACMPNE: {
                Value b;
                Value a;
                if (!popChecked(b) || !popChecked(a)) return Err(InterpFail::StackUnderflow);
                if (a.kind == Value::Kind::Unknown || b.kind == Value::Kind::Unknown) {
                    return Err(InterpFail::NotConcrete);
                }
                const bool same = a.obj == b.obj && (a.kind == Value::Kind::Null) ==
                                                           (b.kind == Value::Kind::Null);
                const bool taken = in->op == Op::IF_ACMPEQ ? same : !same;
                if (taken) {
                    ca::usize t = 0;
                    if (!resolveTarget(in->target, t)) return Err(InterpFail::OutOfRegion);
                    f.pc = t;
                }
                break;
            }
            case Op::TABLESWITCH: case Op::LOOKUPSWITCH: {
                ca::i32 key = 0;
                if (!popInt(key)) return Err(InterpFail::StackUnderflow);
                ca::usize target = order.size();
                bool      found  = false;
                if (in->op == Op::TABLESWITCH) {
                    const ca::i32 low  = in->a;
                    const ca::i32 high = in->b;
                    if (key >= low && key <= high) {
                        const ca::usize i = static_cast<ca::usize>(key - low);
                        if (i < in->targets.size() && resolveTarget(in->targets[i], target)) {
                            found = true;
                        }
                    }
                }
                else {
                    for (ca::usize i = 0; i < in->keys.size() && i < in->targets.size(); ++i) {
                        if (in->keys[i] == key) {
                            if (resolveTarget(in->targets[i], target)) found = true;
                            break;
                        }
                    }
                }
                if (!found && !resolveTarget(in->target, target)) {
                    return Err(InterpFail::OutOfRegion);
                }
                f.pc = target;
                break;
            }

            // ---- 返回 ----
            case Op::IRETURN: case Op::LRETURN: case Op::FRETURN: case Op::DRETURN:
            case Op::ARETURN: case Op::RETURN: {
                if (regionMode) return Err(InterpFail::OutOfRegion);
                return Ok();
            }

            case Op::ATHROW:
                return Err(InterpFail::Runtime);

            // ---- 调用 ----
            case Op::INVOKESTATIC: case Op::INVOKEVIRTUAL: case Op::INVOKESPECIAL:
            case Op::INVOKEINTERFACE: {
                ClassSession::MemberSem sem;
                if (!f.session->memberOf(*in, sem)) return Err(InterpFail::BadOperand);
                std::vector<char> params;
                if (!paramTypes(sem.desc, params)) return Err(InterpFail::BadOperand);
                std::vector<Value> args(params.size());
                for (ca::usize i = params.size(); i-- > 0;) {
                    Value v;
                    if (!popChecked(v)) return Err(InterpFail::StackUnderflow);
                    const bool wantCat2 = params[i] == 'J' || params[i] == 'D';
                    if (v.cat2() != wantCat2) return Err(InterpFail::BadOperand);
                    args[i] = std::move(v);
                }
                Value recv;
                if (in->op != Op::INVOKESTATIC) {
                    if (!popChecked(recv)) return Err(InterpFail::StackUnderflow);
                }
                auto r = invokeCallee(f, *in, [&] {
                    std::vector<Value> all;
                    if (in->op != Op::INVOKESTATIC) all.push_back(std::move(recv));
                    for (auto& a : args) all.push_back(std::move(a));
                    return all;
                }(), f.depth + 1);
                if (r.is_err()) return Err(r.unwrap_err());
                const char rt = retType(sem.desc);
                if (rt != 'V') f.stack.push_back(std::move(r).unwrap());
                break;
            }

            default:
                return Err(InterpFail::Unsupported);
            }
            break;
        }

        if (regionMode && cur == regionEnd) return Ok();
    }

    if (regionMode) return Err(InterpFail::OutOfRegion);
    return Ok();
}

Result<Value, InterpFail> Interp::invokeCallee(Frame& f, const Insn& in, std::vector<Value> args,
                                               int depth)
{
    ClassSession::MemberSem sem;
    if (!f.session->memberOf(in, sem)) return Err(InterpFail::BadOperand);
    if (depth >= 8) return Err(InterpFail::StepLimit);

    const std::string owner(sem.owner);
    const std::string name(sem.name);
    const std::string desc(sem.desc);

    // ── JDK 纯方法内建（解密器与谓词的调用面）──
    const bool isStaticCall = in.op == Op::INVOKESTATIC;
    Value*     recv = isStaticCall ? nullptr : (args.empty() ? nullptr : &args[0]);

    auto argAt = [&](ca::usize i) -> Value& { return args[isStaticCall ? i : i + 1]; };
    auto nargs  = [&]() -> ca::usize { return isStaticCall ? args.size() : args.size() - 1; };

    auto needStr = [&](Value& v, const std::u16string*& out) -> bool {
        out = strOf(v);
        return out != nullptr;
    };

    if (owner == "java/lang/Object" && name == "<init>") return Value{};

    if (owner == "java/lang/String") {
        if (name == "length" && desc == "()I") {
            const std::u16string* p = nullptr;
            if (recv == nullptr || !needStr(*recv, p)) return Err(InterpFail::BadOperand);
            return Value::ofInt(static_cast<ca::i32>(p->size()));
        }
        if (name == "charAt" && desc == "(I)C") {
            const std::u16string* p = nullptr;
            if (recv == nullptr || !needStr(*recv, p)) return Err(InterpFail::BadOperand);
            Value iv = argAt(0);
            if (iv.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
            if (iv.i < 0 || static_cast<ca::usize>(iv.i) >= p->size()) return Err(InterpFail::Runtime);
            return Value::ofInt((*p)[static_cast<ca::usize>(iv.i)]);
        }
        if (name == "toCharArray" && desc == "()[C") {
            const std::u16string* p = nullptr;
            if (recv == nullptr || !needStr(*recv, p)) return Err(InterpFail::BadOperand);
            Value v;
            v.kind = Value::Kind::Chars;
            v.arr  = std::make_shared<std::vector<ca::i32>>();
            v.arr->reserve(p->size());
            for (const char16_t c : *p) v.arr->push_back(static_cast<ca::i32>(c));
            return v;
        }
        if (name == "intern" && desc == "()Ljava/lang/String;") {
            if (recv == nullptr) return Err(InterpFail::BadOperand);
            const std::u16string* p = nullptr;
            if (!needStr(*recv, p)) return Err(InterpFail::BadOperand);
            return Value::ofStr(*p);
        }
        if (name == "valueOf") {
            if (desc == "([C)Ljava/lang/String;" || desc == "([CII)Ljava/lang/String;") {
                Value av = argAt(0);
                if (av.kind != Value::Kind::Chars || !av.arr) return Err(InterpFail::BadOperand);
                ca::usize off = 0;
                ca::usize len = av.arr->size();
                if (desc == "([CII)Ljava/lang/String;") {
                    Value ov = argAt(1);
                    Value lv = argAt(2);
                    if (ov.kind != Value::Kind::Int || lv.kind != Value::Kind::Int) {
                        return Err(InterpFail::BadOperand);
                    }
                    off = static_cast<ca::usize>(ov.i);
                    len = static_cast<ca::usize>(lv.i);
                }
                if (off > av.arr->size() || len > av.arr->size() - off) {
                    return Err(InterpFail::Runtime);
                }
                std::u16string s;
                s.reserve(len);
                for (ca::usize i = 0; i < len; ++i) {
                    s.push_back(static_cast<char16_t>((*av.arr)[off + i]));
                }
                return Value::ofStr(std::move(s));
            }
            if (desc == "(C)Ljava/lang/String;" || desc == "(I)Ljava/lang/String;" ||
                desc == "(J)Ljava/lang/String;")
            {
                Value v = argAt(0);
                if (v.kind == Value::Kind::Int) {
                    return Value::ofStr(decOf(v.i));
                }
                if (v.kind == Value::Kind::Long) {
                    return Value::ofStr(decOf(v.j));
                }
                return Err(InterpFail::BadOperand);
            }
        }
        if (name == "concat" && desc == "(Ljava/lang/String;)Ljava/lang/String;") {
            const std::u16string* a = nullptr;
            const std::u16string* b = nullptr;
            if (recv == nullptr || !needStr(*recv, a) || !needStr(argAt(0), b)) {
                return Err(InterpFail::BadOperand);
            }
            return Value::ofStr(*a + *b);
        }
        if (name == "substring" && (desc == "(I)Ljava/lang/String;" ||
                                    desc == "(II)Ljava/lang/String;"))
        {
            const std::u16string* p = nullptr;
            if (recv == nullptr || !needStr(*recv, p)) return Err(InterpFail::BadOperand);
            Value bv = argAt(0);
            if (bv.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
            ca::usize begin = static_cast<ca::usize>(bv.i);
            ca::usize end   = p->size();
            if (desc == "(II)Ljava/lang/String;") {
                Value ev = argAt(1);
                if (ev.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
                end = static_cast<ca::usize>(ev.i);
            }
            if (begin > end || end > p->size()) return Err(InterpFail::Runtime);
            return Value::ofStr(p->substr(begin, end - begin));
        }
        if (name == "<init>") {
            if (recv == nullptr || !recv->obj) return Err(InterpFail::BadOperand);
            if (recv->obj->kind != HeapObj::Kind::StringInit) return Err(InterpFail::BadOperand);
            if (desc == "([C)V" || desc == "([CII)V") {
                Value av = argAt(0);
                if (av.kind != Value::Kind::Chars || !av.arr) return Err(InterpFail::BadOperand);
                ca::usize off = 0;
                ca::usize len = av.arr->size();
                if (desc == "([CII)V") {
                    Value ov = argAt(1);
                    Value lv = argAt(2);
                    if (ov.kind != Value::Kind::Int || lv.kind != Value::Kind::Int) {
                        return Err(InterpFail::BadOperand);
                    }
                    off = static_cast<ca::usize>(ov.i);
                    len = static_cast<ca::usize>(lv.i);
                }
                if (off > av.arr->size() || len > av.arr->size() - off) {
                    return Err(InterpFail::Runtime);
                }
                recv->obj->text.assign(av.arr->begin() + static_cast<std::ptrdiff_t>(off),
                                       av.arr->begin() + static_cast<std::ptrdiff_t>(off + len));
                recv->obj->kind = HeapObj::Kind::StringVal;
                return Value{};
            }
            if (desc == "(Ljava/lang/String;)V") {
                const std::u16string* p = nullptr;
                if (!needStr(argAt(0), p)) return Err(InterpFail::BadOperand);
                recv->obj->text = *p;
                recv->obj->kind = HeapObj::Kind::StringVal;
                return Value{};
            }
            if (desc == "()V") {
                recv->obj->kind = HeapObj::Kind::StringVal;
                return Value{};
            }
            return Err(InterpFail::Unsupported);
        }
        return Err(InterpFail::Unsupported);
    }

    if (owner == "java/lang/StringBuilder" || owner == "java/lang/StringBuffer") {
        if (name == "<init>") {
            if (recv == nullptr || !recv->obj) return Err(InterpFail::BadOperand);
            recv->obj->kind = HeapObj::Kind::StringBuilder;
            if (desc == "(Ljava/lang/String;)V") {
                const std::u16string* p = nullptr;
                if (!needStr(argAt(0), p)) return Err(InterpFail::BadOperand);
                recv->obj->text = *p;
            }
            return Value{};
        }
        std::u16string* sb = recv ? sbOf(*recv) : nullptr;
        if (sb == nullptr) return Err(InterpFail::BadOperand);
        if (name == "append" || name == "insert") {
            ca::usize pos = sb->size();
            ca::usize argIdx = 0;
            if (name == "insert") {
                Value iv = argAt(0);
                if (iv.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
                if (iv.i < 0 || static_cast<ca::usize>(iv.i) > sb->size()) {
                    return Err(InterpFail::Runtime);
                }
                pos    = static_cast<ca::usize>(iv.i);
                argIdx = 1;
            }
            Value v = argAt(argIdx);
            std::u16string text;
            if (const std::u16string* p = strOf(v)) {
                text = *p;
            }
            else if (v.kind == Value::Kind::Int) {
                text = decOf(v.i);
            }
            else if (v.kind == Value::Kind::Long) {
                text = decOf(v.j);
            }
            else if (v.kind == Value::Kind::Chars && v.arr) {
                for (const ca::i32 c : *v.arr) text.push_back(static_cast<char16_t>(c));
            }
            else {
                return Err(InterpFail::BadOperand);
            }
            sb->insert(pos, text);
            return *recv;
        }
        if (name == "toString" && desc == "()Ljava/lang/String;") {
            return Value::ofStr(*sb);
        }
        if (name == "length" && desc == "()I") {
            return Value::ofInt(static_cast<ca::i32>(sb->size()));
        }
        if (name == "charAt" && desc == "(I)C") {
            Value iv = argAt(0);
            if (iv.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
            if (iv.i < 0 || static_cast<ca::usize>(iv.i) >= sb->size()) return Err(InterpFail::Runtime);
            return Value::ofInt((*sb)[static_cast<ca::usize>(iv.i)]);
        }
        return Err(InterpFail::Unsupported);
    }

    if (owner == "java/util/Random") {
        if (name == "<init>") {
            if (recv == nullptr || !recv->obj) return Err(InterpFail::BadOperand);
            recv->obj->kind = HeapObj::Kind::Random;
            if (desc == "(J)V") {
                Value sv = argAt(0);
                if (sv.kind != Value::Kind::Long) return Err(InterpFail::BadOperand);
                recv->obj->seed = (static_cast<ca::u64>(sv.j) ^ 0x5DEECE66DULL) & kRandomMask;
            }
            else {
                recv->obj->seed = seedBase_ & kRandomMask;
            }
            return Value{};
        }
        if (recv == nullptr || !recv->obj || recv->obj->kind != HeapObj::Kind::Random) {
            return Err(InterpFail::BadOperand);
        }
        ca::u64& seed = recv->obj->seed;
        if (name == "nextInt" && desc == "()I") {
            return Value::ofInt(randomNext(seed, 32));
        }
        if (name == "nextInt" && desc == "(I)I") {
            Value bv = argAt(0);
            if (bv.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
            ca::i32 out = 0;
            if (!randomNextInt(seed, bv.i, out)) return Err(InterpFail::Runtime);
            return Value::ofInt(out);
        }
        if (name == "nextLong" && desc == "()J") {
            const ca::u64 hi = static_cast<ca::u64>(static_cast<ca::i64>(randomNext(seed, 32)));
            const ca::u64 lo = static_cast<ca::u64>(static_cast<ca::i64>(randomNext(seed, 32)));
            return Value::ofLong(static_cast<ca::i64>((hi << 32) + lo));
        }
        if (name == "nextBoolean" && desc == "()Z") {
            return Value::ofInt(randomNext(seed, 1) != 0 ? 1 : 0);
        }
        return Err(InterpFail::Unsupported);
    }

    if (owner == "java/lang/Math") {
        if (desc == "(I)I") {
            Value a = argAt(0);
            if (a.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
            if (name == "abs") return Value::ofInt(a.i < 0 ? -a.i : a.i);
            if (name == "negateExact") return Value::ofInt(-a.i);
            return Err(InterpFail::Unsupported);
        }
        if (desc == "(II)I") {
            Value a = argAt(0);
            Value b = argAt(1);
            if (a.kind != Value::Kind::Int || b.kind != Value::Kind::Int) {
                return Err(InterpFail::BadOperand);
            }
            if (name == "min") return Value::ofInt(a.i < b.i ? a.i : b.i);
            if (name == "max") return Value::ofInt(a.i > b.i ? a.i : b.i);
            return Err(InterpFail::Unsupported);
        }
        if (desc == "(J)J") {
            Value a = argAt(0);
            if (a.kind != Value::Kind::Long) return Err(InterpFail::BadOperand);
            if (name == "abs") return Value::ofLong(a.j < 0 ? -a.j : a.j);
            return Err(InterpFail::Unsupported);
        }
        return Err(InterpFail::Unsupported);
    }

    if (owner == "java/lang/Integer") {
        if (name == "parseInt" && desc == "(Ljava/lang/String;)I") {
            const std::u16string* p = nullptr;
            if (!needStr(argAt(0), p)) return Err(InterpFail::BadOperand);
            std::string ascii;
            for (const char16_t c : *p) {
                if (c > 0x7F) return Err(InterpFail::BadOperand);
                ascii.push_back(static_cast<char>(c));
            }
            try {
                return Value::ofInt(static_cast<ca::i32>(std::stol(ascii)));
            }
            catch (...) {
                return Err(InterpFail::Runtime);
            }
        }
        if (name == "toString" && desc == "(I)Ljava/lang/String;") {
            Value v = argAt(0);
            if (v.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
            return Value::ofStr(decOf(v.i));
        }
        return Err(InterpFail::Unsupported);
    }

    if (owner == "java/lang/Character" && name == "toString" &&
        desc == "(C)Ljava/lang/String;")
    {
        Value v = argAt(0);
        if (v.kind != Value::Kind::Int) return Err(InterpFail::BadOperand);
        std::u16string s;
        s.push_back(static_cast<char16_t>(v.i));
        return Value::ofStr(std::move(s));
    }

    if (owner == "java/lang/System" && name == "arraycopy" &&
        desc == "(Ljava/lang/Object;ILjava/lang/Object;II)V")
    {
        Value src = argAt(0);
        Value sp  = argAt(1);
        Value dst = argAt(2);
        Value dp  = argAt(3);
        Value ln  = argAt(4);
        if (src.kind != Value::Kind::Chars || dst.kind != Value::Kind::Chars) {
            return Err(InterpFail::Unsupported);
        }
        if (sp.kind != Value::Kind::Int || dp.kind != Value::Kind::Int ||
            ln.kind != Value::Kind::Int)
        {
            return Err(InterpFail::BadOperand);
        }
        if (sp.i < 0 || dp.i < 0 || ln.i < 0 ||
            static_cast<ca::usize>(sp.i) + static_cast<ca::usize>(ln.i) > src.arr->size() ||
            static_cast<ca::usize>(dp.i) + static_cast<ca::usize>(ln.i) > dst.arr->size())
        {
            return Err(InterpFail::Runtime);
        }
        for (ca::i32 i = 0; i < ln.i; ++i) {
            (*dst.arr)[static_cast<ca::usize>(dp.i + i)] =
                (*src.arr)[static_cast<ca::usize>(sp.i + i)];
        }
        return Value{};
    }

    // ── 容器内纯方法：递归解释 ──
    const auto clsIt = classes_.find(owner);
    if (clsIt == classes_.end() || clsIt->second == nullptr) return Err(InterpFail::Unsupported);
    ClassSession* callee = clsIt->second;

    ca::usize calleeIdx = static_cast<ca::usize>(-1);
    const auto& methods = callee->unit().methods;
    for (ca::usize i = 0; i < methods.size(); ++i) {
        if (callee->unit().symbols().str(methods[i].name) == name &&
            callee->unit().symbols().str(methods[i].desc) == desc)
        {
            calleeIdx = i;
            break;
        }
    }
    if (calleeIdx == static_cast<ca::usize>(-1)) return Err(InterpFail::Unsupported);
    if (!pure_(*callee, calleeIdx)) return Err(InterpFail::Unsupported);

    return invokeMethod(*callee, calleeIdx, args);
}

Result<Value, InterpFail> Interp::invokeMethod(ClassSession& session, ca::usize methodIdx,
                                               const std::vector<Value>& args)
{
    if (depth_ >= 8) return Err(InterpFail::StepLimit);
    if (!jbc::ir::hasCode(session, methodIdx)) return Err(InterpFail::Unsupported);
    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) return Err(InterpFail::Unsupported);
    jbc::ir::CodeUnitRef unitRef = std::move(promoted).unwrap();

    Frame f;
    f.session = &session;
    f.unit    = unitRef.get();
    f.order   = &unitRef->dom.order();
    f.depth   = depth_;
    for (ca::usize i = 0; i < f.order->size(); ++i) f.indexOf[(*f.order)[i].slot] = i;

    // 播种实参：静态方法槽 0 起；实例方法槽 0 为 this。
    const jbc::ir::MethodUnit& mu = session.unit().methods[methodIdx];
    const bool isStatic = (mu.access & 0x0008u) != 0;
    ca::i32    slot     = isStatic ? 0 : 1;
    for (const Value& a : args) {
        f.locals[slot] = a;
        slot += a.cat2() ? 2 : 1;
    }

    ++depth_;
    const auto r = run(f, false, 0);
    --depth_;
    if (r.is_err()) return Err(r.unwrap_err());

    // 取返回值：return 前栈顶即返回值。
    if (f.stack.empty()) {
        if (retType(session.unit().symbols().str(mu.desc)) == 'V') return Value{};
        return Err(InterpFail::Runtime);
    }
    return f.stack.back();
}

Result<std::vector<Value>, InterpFail> Interp::runRegion(ClassSession& session, CodeUnit& unit,
                                                         ca::usize start, ca::usize end,
                                                         std::vector<bool>* executed,
                                                         ca::usize exitOk)
{
    const auto& order = unit.dom.order();
    if (start >= order.size() || end >= order.size() || start > end) {
        return Err(InterpFail::BadOperand);
    }

    Frame f;
    f.session     = &session;
    f.unit        = &unit;
    f.order       = &order;
    f.pc          = start;
    f.regionMode  = true;
    f.regionStart = start;
    f.regionEnd   = end;
    f.exitOk      = exitOk;
    f.executed    = executed;
    for (ca::usize i = 0; i < order.size(); ++i) f.indexOf[order[i].slot] = i;
    if (executed != nullptr) executed->assign(order.size(), false);

    const auto r = run(f, true, end);
    if (r.is_err()) return Err(r.unwrap_err());
    return f.stack;
}

}   // namespace dasho
