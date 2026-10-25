/// DashoCleanupPass 单测：夹具手工拼 DashO 控制流填充的**五种产物形态**，断言
/// 判据的边界。
///
/// 核心判据是"空转窗口"：一段栈自足（深度从 0 起、全程非负、收在 goto 前回到
/// 0）、写下的局部变量槽在出口之后已死（活跃性分析）、内部没有块首、且那条
/// `goto` 不跳回窗口内的连续指令。
///
///   - **正例**：DashO 的 `ldc -1; istore N; ldc "0"; iinc N 1; astore N+1` 被
///     整段删掉，方法回到原逻辑。
///   - **写的槽还活着**：窗口写到的槽在出口后仍被读 → 不动。这是 dead-store
///     判据的反例，也是"全局不读"判据会漏掉的那类（填充复用真变量槽）。
///   - **吃入口栈**：`pop; aconst_null` 净效应 0 但把入口栈顶换成了 null →
///     不动。只按"净效应 0"判会删错。
///   - **不可达填充**：`goto L; nop; athrow; L:` 死块被剪除。
///   - **带异常表**：整方法跳过，一个字节都不动。
///   - **`goto` 跳回窗口首**：删了会变自环 → 不动。窗口内部无块首这条挡不住
///     它（跳回的是首条而非中间某条），必须显式判落点。
///
/// 语料（`SnakeGame-cf-blocks.jar` / `SnakeGame-all.jar`）的还原结果由语料
/// 回归覆盖。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/code_dom.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "dasho/cleanup_pass.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u8 kNop        = 0x00;
constexpr ca::u8 kAconstNull = 0x01;
constexpr ca::u8 kIconst1    = 0x04;
constexpr ca::u8 kLdc        = 0x12;
constexpr ca::u8 kIload1     = 0x1b;
constexpr ca::u8 kIstore1    = 0x3c;
constexpr ca::u8 kAstore2    = 0x4d;
constexpr ca::u8 kPop        = 0x57;
constexpr ca::u8 kIinc       = 0x84;
constexpr ca::u8 kGoto       = 0xa7;
constexpr ca::u8 kIreturn    = 0xac;
constexpr ca::u8 kAreturn    = 0xb0;
constexpr ca::u8 kAthrow     = 0xbf;

void emitU16(Bytes& out, ca::i32 v)
{
    out.push_back(static_cast<ca::u8>(static_cast<ca::u16>(v) >> 8));
    out.push_back(static_cast<ca::u8>(v));
}

void emitLdc(Bytes& out, ca::u16 idx)
{
    out.push_back(kLdc);
    out.push_back(static_cast<ca::u8>(idx));
}

/// 分支偏移相对**分支指令自身的 pc**（JVMS 6.5）。
void emitBranch(Bytes& out, ca::u8 op, ca::i32 targetPc)
{
    const ca::i32 off = targetPc - static_cast<ca::i32>(out.size());
    out.push_back(op);
    emitU16(out, off);
}

/// `istore N` / `iinc N, 1` 的显式索引形态（N >= 4 时必须用宽形态）。
void emitIstore(Bytes& out, ca::u8 slot)
{
    if (slot <= 3) {
        out.push_back(static_cast<ca::u8>(kIstore1 + (slot - 1)));   // 1..3 的缩略形态
        return;
    }
    out.push_back(0x36);
    out.push_back(slot);
}

void emitIinc(Bytes& out, ca::u8 slot, ca::i8 incr)
{
    out.push_back(kIinc);
    out.push_back(slot);
    out.push_back(static_cast<ca::u8>(incr));
}

/// DashO 的填充块（6 条，`controlflow blockSplitting` / `jumbling` 的产物）。
void emitFill(Bytes& out, ca::u8 slot, ca::u16 minusOne, ca::u16 zero)
{
    emitLdc(out, minusOne);
    emitIstore(out, slot);
    emitLdc(out, zero);
    emitIinc(out, slot, 1);
    out.push_back(kAstore2);
}

mjbase::spi::PreservedAnalyses runPass(mjbase::model::ResourceContainer& container)
{
    dasho::DashoCleanupPass   pass;
    ca::str::Utf8StringArena  arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    return pass.run(container, gam);
}

struct Decoded
{
    std::vector<ca::u8> ops;
    ca::usize           catchEntries = 0;
};

bool decode(mjbase::model::ResourceContainer& container, const char* uri, const char* name,
            Decoded& out)
{
    auto* resource = container.getResource(uri);
    if (resource == nullptr) return false;
    auto* cls = ca::core::tag_cast::cast<mjbase::model::ClassResource>(resource);
    if (cls == nullptr) return false;
    auto read = jbc::core::readClass(jbc::core::ByteView(cls->get()));
    if (read.is_err()) return false;
    auto file = std::move(read).unwrap().file;

    jbc::core::MemberInfo* target = nullptr;
    for (jbc::core::MemberInfo& m : file->methods) {
        if (file->nameOf(m) == name) target = &m;
    }
    if (target == nullptr) return false;

    auto decoded = file->decodeCode(*target);
    if (decoded.is_err()) return false;
    const jbc::core::CodeAttr* code = std::move(decoded).unwrap();
    if (code == nullptr) return false;
    out.catchEntries = code->excTable.size();

    auto domRes = jbc::ir::CodeDom::importFrom(jbc::core::ByteView(code->code));
    if (domRes.is_err()) return false;
    jbc::ir::CodeDom dom = std::move(domRes).unwrap();
    for (const jbc::ir::InsnId id : dom.order()) {
        const jbc::ir::Insn* in = dom.peek(id);
        if (in == nullptr) return false;
        out.ops.push_back(static_cast<ca::u8>(in->op));
    }
    return true;
}

/// 正例夹具：`return "ok"`，填充插在 `ldc "ok"` 与 `areturn` 之间。
///
/// ```
///   0  ldc "ok"
///   2  ldc -1 ; istore_1 ; ldc "0" ; iinc 1,1 ; astore_2   ; 填充
///  11  goto 13
///  13  areturn
/// ```
Bytes buildFillBeforeReturn(const char* name, bool slotLiveAfter)
{
    FixtureBuilder builder(name);
    const ca::u16  okText   = builder.strConst("ok");
    const ca::u16  zeroText = builder.strConst("0");
    const ca::u16  minusOne = builder.intConst(-1);

    Bytes code;
    if (slotLiveAfter) {
        // 先把 7 写进槽 1，让槽 1 在填充之后仍然活着（后面要读）。
        code.push_back(0x10);   // bipush 7
        code.push_back(0x07);
        code.push_back(kIstore1);
    }
    emitLdc(code, okText);      // 0（slotLiveAfter 时为 3）
    emitFill(code, 1, minusOne, zeroText);
    emitBranch(code, kGoto, static_cast<ca::i32>(code.size()) + 3);
    if (slotLiveAfter) {
        code.push_back(kIload1);
        code.push_back(kIreturn);
    } else {
        code.push_back(kAreturn);
    }

    FixtureMethod run;
    run.name      = "run";
    run.desc      = slotLiveAfter ? "()I" : "()Ljava/lang/String;";
    run.access    = 0x0009;
    run.maxStack  = 2;
    run.maxLocals = 3;
    run.code      = std::move(code);
    builder.addMethod(run);
    return builder.assemble();
}

/// 反例夹具：`pop; aconst_null` 净效应 0，但吃掉了入口栈上的值。
Bytes buildEntryStackConsumer(const char* name)
{
    FixtureBuilder builder(name);
    const ca::u16  okText = builder.strConst("ok");

    Bytes code;
    emitLdc(code, okText);      // 0
    code.push_back(kPop);       // 2  吃入口栈
    code.push_back(kAconstNull);// 3  换成 null
    emitBranch(code, kGoto, static_cast<ca::i32>(code.size()) + 3);   // 4
    code.push_back(kAreturn);   // 7

    FixtureMethod run;
    run.name      = "run";
    run.desc      = "()Ljava/lang/Object;";
    run.access    = 0x0009;
    run.maxStack  = 1;
    run.maxLocals = 1;
    run.code      = std::move(code);
    builder.addMethod(run);
    return builder.assemble();
}

/// 不可达填充：`goto L; nop; athrow; nop; L:`。
Bytes buildDeadFill(const char* name)
{
    FixtureBuilder builder(name);
    const ca::u16  okText = builder.strConst("ok");

    Bytes code;
    emitBranch(code, kGoto, 6);   // 0
    code.push_back(kNop);         // 3
    code.push_back(kAthrow);      // 4
    code.push_back(kNop);         // 5
    emitLdc(code, okText);        // 6
    code.push_back(kAreturn);     // 8

    FixtureMethod run;
    run.name      = "run";
    run.desc      = "()Ljava/lang/String;";
    run.access    = 0x0009;
    run.maxStack  = 1;
    run.maxLocals = 1;
    run.code      = std::move(code);
    builder.addMethod(run);
    return builder.assemble();
}

/// 带真 handler 的方法：填充一模一样，但异常表非空 → 整方法跳过。
Bytes buildWithExceptionTable(const char* name)
{
    FixtureBuilder builder(name);
    const ca::u16  okText   = builder.strConst("ok");
    const ca::u16  zeroText = builder.strConst("0");
    const ca::u16  minusOne = builder.intConst(-1);
    const ca::u16  throwable = builder.cls("java/lang/Exception");

    Bytes code;
    emitLdc(code, okText);                                   // 0
    emitFill(code, 1, minusOne, zeroText);                   // 2
    emitBranch(code, kGoto, static_cast<ca::i32>(code.size()) + 3);
    code.push_back(kAreturn);                                // 13

    FixtureMethod run;
    run.name      = "run";
    run.desc      = "()Ljava/lang/String;";
    run.access    = 0x0009;
    run.maxStack  = 2;
    run.maxLocals = 3;
    run.code      = std::move(code);
    run.exc       = {{0, 13, 13, throwable}};
    builder.addMethod(run);
    return builder.assemble();
}

/// 窗口收在一条**跳回窗口首**的 `goto` 上。窗口内部没有块首（跳回的是首条
/// 而非中间某条），所以"内部无块首"这条挡不住它；必须显式判 `goto` 的落点。
/// 删掉窗口会让这条 `goto` 变成自环。
///
/// 窗口刻意做成"任何真前缀都不净 0"（`ldc; ldc; istore; istore` 的累计深度是
/// `1,2,1,0`），否则判据会退而从后缀起算——后缀一旦自足就绕过了守卫。
///
/// ```
///   0  ldc "ok"
///   1  ldc -1 ; ldc "0" ; istore_1 ; istore_2   ; 窗口
///   6  goto 1
/// ```
Bytes buildBackEdgeWindow(const char* name)
{
    FixtureBuilder builder(name);
    const ca::u16  okText   = builder.strConst("ok");
    const ca::u16  zeroText = builder.strConst("0");
    const ca::u16  minusOne = builder.intConst(-1);

    Bytes code;
    emitLdc(code, okText);                                     // 0
    const ca::i32 windowStart = static_cast<ca::i32>(code.size());   // 2
    emitLdc(code, minusOne);                                   // 2
    emitLdc(code, zeroText);                                   // 4
    emitIstore(code, 1);                                       // 6
    emitIstore(code, 2);                                       // 7
    emitBranch(code, kGoto, windowStart);                      // 8

    FixtureMethod run;
    run.name      = "run";
    run.desc      = "()V";
    run.access    = 0x0009;
    run.maxStack  = 2;
    run.maxLocals = 3;
    run.code      = std::move(code);
    builder.addMethod(run);
    return builder.assemble();
}

}   // namespace

/// 正例：填充整段删除，`goto` 到紧邻下一条也被清掉，方法回到 `ldc; areturn`。
TEST(MjPassDashoCleanupTest, BypassesDeadStoreFill)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Host.class", buildFillBeforeReturn("pkg/Host", false));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ops[0], kLdc);
    EXPECT_EQ(d.ops[1], kAreturn);
}

/// 反例：填充写到的槽在出口之后仍被读 → 一个字节都不动。
TEST(MjPassDashoCleanupTest, KeepsFillWritingLiveSlot)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Host.class", buildFillBeforeReturn("pkg/Host", true));

    const auto pa = runPass(container);
    // `goto` 到紧邻下一条仍会被清掉，但填充必须原样留着。
    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    EXPECT_NE(std::find(d.ops.begin(), d.ops.end(), kIinc), d.ops.end());
    EXPECT_NE(std::find(d.ops.begin(), d.ops.end(), kIstore1), d.ops.end());
    // bipush + istore_1 + ldc + 填充 5 条 + iload_1 + ireturn（goto 到紧邻下一条被清掉）
    EXPECT_EQ(d.ops.size(), 10u);
}

/// 反例：`pop; aconst_null` 净效应 0，但把入口栈顶换成了 null → 不动。
TEST(MjPassDashoCleanupTest, KeepsWindowConsumingEntryStack)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Host.class", buildEntryStackConsumer("pkg/Host"));

    const auto pa = runPass(container);
    static_cast<void>(pa);

    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    EXPECT_NE(std::find(d.ops.begin(), d.ops.end(), kPop), d.ops.end());
    EXPECT_NE(std::find(d.ops.begin(), d.ops.end(), kAconstNull), d.ops.end());
}

/// 不可达填充块被剪除，剩下的 `goto` 到紧邻下一条也一并清掉。
TEST(MjPassDashoCleanupTest, PrunesUnreachableFill)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Host.class", buildDeadFill("pkg/Host"));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ops[0], kLdc);
    EXPECT_EQ(d.ops[1], kAreturn);
}

/// 带异常表的方法整体跳过：填充在，异常表也在，字节不动。
TEST(MjPassDashoCleanupTest, SkipsMethodWithExceptionTable)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Host.class", buildWithExceptionTable("pkg/Host"));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    EXPECT_EQ(d.catchEntries, 1u);
    EXPECT_NE(std::find(d.ops.begin(), d.ops.end(), kIinc), d.ops.end());
}

/// 反例：`goto` 跳回窗口首 → 删了会变自环，一条都不动。
TEST(MjPassDashoCleanupTest, KeepsWindowWithBackEdgeGoto)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Host.class", buildBackEdgeWindow("pkg/Host"));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    // ldc "ok" + 窗口 4 条 + goto，一条不少。
    ASSERT_EQ(d.ops.size(), 6u);
    EXPECT_EQ(d.ops[0], kLdc);
    EXPECT_NE(std::find(d.ops.begin(), d.ops.end(), kIstore1), d.ops.end());
    EXPECT_EQ(d.ops.back(), kGoto);
}
