/// @file        jvm_interp.hpp
/// @brief       常量求值用的小型 JVM 字节码解释器（值域 = 具体值 + Unknown）。
///
/// 面向 DashO 字符串加密的还原：解密器与不透明谓词都是**纯**静态方法，
/// 调用点是一段直线（含一个菱形）的常量算术。本解释器只求值这类「可静态
/// 判定」的方法体与指令区间，遇到不可判定的形态（字段读写、未知调用、
/// 未知输入）一律失败返回，由调用方跳过该站点——不做任何猜测性求值。
///
/// 两种运行模式：
///   - 方法模式（invokeMethod）：按签名播种实参，跑到 return；用于解密器
///     与不透明谓词源。
///   - 区间模式（runRegion）：从第 start 条指令起以空栈运行，执行完第 end
///     条即停；用于调用点整段折叠（外层密钥算术 + 谓词 + 双支 + 解密调用）。
///     区间外的跳转视为失败（区间必须自洽）。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/ir/units.hpp"

namespace dasho {

/// 解释器运行时值。引用类值用 shared_ptr 表达 JVM 引用语义（dup 复制句柄，
/// <init> 原地变更对同一对象的所有句柄可见）。
struct Value
{
    enum class Kind : ca::u8 {
        Unknown,   ///< 不可判定（未播种的局部变量、未支持的运算结果）
        Null,      ///< aconst_null
        Int,       ///< int / char / boolean / byte / short（统一 i32）
        Long,      ///< long（cat-2）
        Float,     ///< float（仅参与栈操作，不解释算术）
        Double,    ///< double（cat-2，同上）
        Str,       ///< java/lang/String 常量（不可变）
        Chars,     ///< char[]（元素 i32，castore 截断 u16、caload 零扩展）
        Obj,       ///< 其它引用：见 HeapObj
    };

    Kind kind = Kind::Unknown;
    ca::i32 i = 0;
    ca::i64 j = 0;
    double  d = 0.0;
    std::u16string s;                              ///< Str 内容
    std::shared_ptr<std::vector<ca::i32>> arr;     ///< Chars 元素
    std::shared_ptr<struct HeapObj>       obj;     ///< Obj 载荷

    bool cat2() const noexcept { return kind == Kind::Long || kind == Kind::Double; }

    static Value ofInt(ca::i32 v);
    static Value ofLong(ca::i64 v);
    static Value ofStr(std::u16string v);
};

/// 堆对象：StringBuilder 缓冲 / Random 状态 / 构造中的 String。
struct HeapObj
{
    enum class Kind : ca::u8 {
        StringBuilder,   ///< 可变 UTF-16 缓冲
        Random,          ///< java/util/Random（LCG 状态）
        StringInit,      ///< new String 后、<init> 前
        StringVal,       ///< <init> 后的 String 实例
        Plain,           ///< 其它（只支持 <init> 空操作）
    };
    Kind          kind = Kind::Plain;
    std::u16string text;      ///< StringBuilder / StringVal
    ca::u64       seed = 0;   ///< Random 的 48 位状态
};

enum class InterpFail : ca::u8 {
    Ok = 0,
    Unsupported,      ///< 指令/调用不在支持面内
    StackUnderflow,
    BadOperand,       ///< 操作数与指令形状不符
    Runtime,          ///< 除零/越界等运行期错误
    StepLimit,        ///< 步数或递归深度超限
    OutOfRegion,      ///< 区间模式跳出了区间
    NotConcrete,      ///< 结果依赖未知输入
};

const char* to_string(InterpFail f) noexcept;

/// 是否为解释器内建的 JDK 纯方法。纯度判定（pass 侧）与解释（本文件）共用
/// 同一张表：表外的调用一律视为有副作用，不参与求值。
bool isIntrinsicCall(std::string_view owner, std::string_view name, std::string_view desc);

/// 解释器：持类表（内部名 → 会话）与纯度判定回调。
///
/// 纯度回调回答「该方法是否可以安全求值」——由 pass 侧按「无字段写、无同步、
/// 调用面全部纯」递归判定；解释器只在纯方法上递归，绝不执行有副作用代码。
class Interp
{
public:
    using ClassMap  = std::unordered_map<std::string, jbc::ir::ClassSession*>;
    using PureQuery = std::function<bool(jbc::ir::ClassSession&, ca::usize)>;

    struct Limits
    {
        int maxSteps = 200000;
        int maxDepth = 8;
    };

    Interp(const ClassMap& classes, PureQuery pure, ca::u64 seedBase);

    /// 方法模式：以 args 播种，执行到 return。失败即不可判定。
    ca::core::Result<Value, InterpFail> invokeMethod(jbc::ir::ClassSession& session,
                                                    ca::usize            methodIdx,
                                                    const std::vector<Value>& args);

    /// 区间模式：从 order[start] 起空栈执行，执行完 order[end] 停止并返回
    /// 当时的栈。executed（可选）按 order 下标回填「是否被执行过」。
    ///
    /// exitOk（可选）是「区间外但合法的出口」：控制流转移到该指令即视为区间
    /// 正常结束（该指令本身不执行）。求「区间内实参栈」时用它——分支模板常以
    /// `goto 锚点` 收尾，锚点正好在区间外一格。
    ca::core::Result<std::vector<Value>, InterpFail> runRegion(jbc::ir::ClassSession& session,
                                                              jbc::ir::CodeUnit&     unit,
                                                              ca::usize              start,
                                                              ca::usize              end,
                                                              std::vector<bool>*     executed,
                                                              ca::usize              exitOk =
                                                                  static_cast<ca::usize>(-1));

private:
    struct Frame;

    ca::core::Result<void, InterpFail> run(Frame& frame, bool regionMode, ca::usize regionEnd);
    ca::core::Result<Value, InterpFail> invokeCallee(Frame& frame, const jbc::ir::Insn& in,
                                                     std::vector<Value> args, int depth);

    const ClassMap& classes_;
    PureQuery       pure_;
    ca::u64         seedBase_;
    int             depth_ = 0;   ///< 递归解释深度（纯方法链的环由纯度判定先挡）
};

}   // namespace dasho
