/// DashoControlFlowPass 单测：夹具手工拼 DashO `controlflow tryCatch` 的**注入
/// 产物**（头部 goto + 死 athrow 填充 + 尾部填充 + 吞异常 handler + 指向假异常
/// 类的异常表项），断言两条改写策略的分工与"假异常类"判据的边界：
///
///   - **结构改写**（摘完只剩空异常表）：填充段与 handler 体一并删除，头部的
///     `goto L1` 也清掉，假异常类从容器里消失。
///   - **保布局改写**（方法还有真 handler）：只摘注入项，一条指令都不动——
///     Strict 域重算的 handler 帧比 verifier 保守帧更精确，重算即 VerifyError。
///   - **判据边界**：版本戳 45.3 但带成员的不是假异常类；被指令显式引用
///     （new/checkcast）的假异常类不能删。
///
/// 语料形态与还原结果由 `corpus-regress` 覆盖（run-lab cf-trycatch / cf-full +
/// JVM 校验器 + headless 栈迹比对）。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "dasho/control_flow_pass.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u8 kAthrow       = 0xBF;
constexpr ca::u8 kNop          = 0x00;
constexpr ca::u8 kGoto         = 0xA7;
constexpr ca::u8 kAreturn      = 0xB0;
constexpr ca::u8 kReturn       = 0xB1;
constexpr ca::u8 kLdc          = 0x12;
constexpr ca::u8 kAstore0      = 0x4B;
constexpr ca::u8 kAload0       = 0x2A;
constexpr ca::u8 kNew          = 0xBB;
constexpr ca::u8 kPop          = 0x57;
constexpr ca::u8 kInvokestatic = 0xB8;
constexpr ca::u8 kInvokevirtual = 0xB6;

/// DashO 生成类的版本戳：classfile **45.3**。FixtureBuilder 固定写 52.0，
/// 这里按 classfile 头部布局（magic u32 / minor u2 / major u2）回填。
Bytes withVersion45_3(Bytes bytes)
{
    bytes[4] = 0;
    bytes[5] = 3;    // minor = 3
    bytes[6] = 0;
    bytes[7] = 45;   // major = 45
    return bytes;
}

/// 假异常类：0 字段 / 0 方法 / super=RuntimeException / 45.3。
Bytes buildFakeExceptionClass(const char* name, const char* super = "java/lang/RuntimeException")
{
    FixtureBuilder builder(name);
    builder.setSuper(super);
    return withVersion45_3(builder.assemble());
}

void emitGoto(Bytes& out, ca::i32 targetPc)
{
    // JVMS 6.5：偏移相对**分支指令自身的 pc**。
    const ca::i32 off = targetPc - static_cast<ca::i32>(out.size());
    out.push_back(kGoto);
    out.push_back(static_cast<ca::u8>(static_cast<ca::u16>(off) >> 8));
    out.push_back(static_cast<ca::u8>(off));
}

/// 注入形态（一个方法体）：
///
/// ```
///  0: goto L1              ; 跳过死填充
///  3: athrow  nop nop athrow athrow
/// L1: <真代码: ldc "hi"; areturn>
///     nop nop athrow athrow
///     astore_0; return    ; handler
/// ```
///
/// `genuineCatchCp` 非 0 时再加一条指向真异常类的项（同一 handler），用来覆盖
/// "方法还剩真 handler"的保布局路径。
Bytes buildInjectedOwner(const char* owner, const char* fakeName, ca::u16 genuineCatchCp = 0)
{
    FixtureBuilder builder(owner);
    const ca::u16  fakeCp = builder.cls(fakeName);
    const ca::u16  text   = builder.strConst("hi");

    Bytes code;
    emitGoto(code, 8);                     // 0
    code.push_back(kAthrow);               // 3
    code.push_back(kNop);                  // 4
    code.push_back(kNop);                  // 5
    code.push_back(kAthrow);               // 6
    code.push_back(kAthrow);               // 7
    code.push_back(kLdc);                  // 8
    code.push_back(static_cast<ca::u8>(text));
    code.push_back(kAreturn);              // 10
    code.push_back(kNop);                  // 11
    code.push_back(kNop);                  // 12
    code.push_back(kAthrow);               // 13
    code.push_back(kAthrow);               // 14
    code.push_back(kAstore0);              // 15
    code.push_back(kReturn);               // 16

    FixtureMethod m;
    m.name      = "call";
    m.desc      = "()Ljava/lang/String;";
    m.access    = 0x0009;
    m.maxStack  = 1;
    m.maxLocals = 1;
    m.code      = std::move(code);
    m.exc       = {{8, 11, 15, fakeCp}};
    if (genuineCatchCp != 0) {
        m.exc.push_back(ExcSpec{8, 11, 15, genuineCatchCp});
    }
    builder.addMethod(m);
    return builder.assemble();
}

/// 反汇编结果：op 序列 + 异常表（按表序的 catch 类型名）。
struct DecodedMethod
{
    std::vector<ca::u8>      ops;
    std::vector<std::string> catchNames;
};

bool decodeMethod(mjbase::model::ResourceContainer& container, const char* uri, const char* name,
                  DecodedMethod& out)
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

    for (const jbc::core::CodeAttr::ExcEntry& e : code->excTable) {
        out.catchNames.emplace_back(file->className(e.catchType));
    }

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

mjbase::spi::PreservedAnalyses runPass(mjbase::model::ResourceContainer& container)
{
    dasho::DashoControlFlowPass        pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    return pass.run(container, gam);
}

ca::usize countOp(const DecodedMethod& d, ca::u8 op)
{
    ca::usize n = 0;
    for (const ca::u8 o : d.ops) {
        if (o == op) ++n;
    }
    return n;
}

}   // namespace

/// 注入项是唯一异常表项 → 结构改写：填充段、handler、头部 goto 全部消失。
TEST(MjPassDashoControlFlowTest, PrunesFillAndHandlerWhenTableEmpties)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Fake.class", buildFakeExceptionClass("pkg/Fake"));
    addClassResource(container, "pkg/Owner.class", buildInjectedOwner("pkg/Owner", "pkg/Fake"));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    DecodedMethod d;
    ASSERT_TRUE(decodeMethod(container, "pkg/Owner.class", "call", d));
    EXPECT_TRUE(d.catchNames.empty());
    // 只剩真代码：ldc "hi" + areturn。死 athrow 与 handler 的 astore/return 都没了。
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ops[0], kLdc);
    EXPECT_EQ(d.ops[1], kAreturn);
    EXPECT_EQ(countOp(d, kAthrow), 0u);
    EXPECT_EQ(countOp(d, kGoto), 0u);

    // 假异常类不再被任何帧/异常表引用 → 从容器里摘掉。
    EXPECT_FALSE(container.hasResource("pkg/Fake.class"));
}

/// 方法还有真 handler → 保布局改写：只摘注入项，指令一条不动，假类保留。
TEST(MjPassDashoControlFlowTest, KeepsLayoutWhenGenuineHandlerRemains)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Fake.class", buildFakeExceptionClass("pkg/Fake"));

    FixtureBuilder owner("pkg/Owner");
    const ca::u16  fakeCp   = owner.cls("pkg/Fake");
    const ca::u16  realCp   = owner.cls("java/lang/Exception");
    const ca::u16  text     = owner.strConst("hi");
    const ca::u16  printRef = owner.methodRef("java/lang/Throwable", "printStackTrace", "()V");

    Bytes code;
    emitGoto(code, 8);                                  // 0
    code.push_back(kAthrow);                            // 3
    code.push_back(kNop);                               // 4
    code.push_back(kNop);                               // 5
    code.push_back(kAthrow);                            // 6
    code.push_back(kAthrow);                            // 7
    code.push_back(kLdc);                               // 8
    code.push_back(static_cast<ca::u8>(text));
    code.push_back(kAreturn);                           // 10
    emitGoto(code, 19);                                 // 11
    code.push_back(kAstore0);                           // 14
    code.push_back(kAload0);                            // 15
    code.push_back(kInvokevirtual);                     // 16
    code.push_back(static_cast<ca::u8>(printRef >> 8));
    code.push_back(static_cast<ca::u8>(printRef));
    code.push_back(kReturn);                            // 19

    FixtureMethod m;
    m.name      = "call";
    m.desc      = "()Ljava/lang/String;";
    m.access    = 0x0009;
    m.maxStack  = 1;
    m.maxLocals = 1;
    m.code      = std::move(code);
    m.exc       = {ExcSpec{8, 11, 15, fakeCp}, ExcSpec{8, 11, 14, realCp}};
    owner.addMethod(m);
    addClassResource(container, "pkg/Owner.class", owner.assemble());

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    DecodedMethod d;
    ASSERT_TRUE(decodeMethod(container, "pkg/Owner.class", "call", d));
    ASSERT_EQ(d.catchNames.size(), 1u);
    EXPECT_EQ(d.catchNames[0], "java/lang/Exception");
    // 保布局：指令条数与原样一致（goto + 5 条死填充 + 真代码 + 尾巴 + handler）。
    EXPECT_EQ(d.ops.size(), 13u);
    EXPECT_EQ(countOp(d, kAthrow), 3u);

    // handler 帧里还写着假异常类，类必须留着。
    EXPECT_TRUE(container.hasResource("pkg/Fake.class"));
}

/// 只有真 handler、没有注入项 → 完全不动（判据不误报）。
TEST(MjPassDashoControlFlowTest, IgnoresGenuineOnlyMethod)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Fake.class", buildFakeExceptionClass("pkg/Fake"));

    FixtureBuilder owner("pkg/Owner");
    const ca::u16  realCp   = owner.cls("java/lang/Exception");
    const ca::u16  text     = owner.strConst("hi");
    const ca::u16  printRef = owner.methodRef("java/lang/Throwable", "printStackTrace", "()V");

    Bytes code;
    code.push_back(kLdc);                               // 0
    code.push_back(static_cast<ca::u8>(text));
    code.push_back(kAreturn);                           // 2
    emitGoto(code, 6);                                  // 3
    code.push_back(kAstore0);                           // 6
    code.push_back(kAload0);                            // 7
    code.push_back(kInvokevirtual);                     // 8
    code.push_back(static_cast<ca::u8>(printRef >> 8));
    code.push_back(static_cast<ca::u8>(printRef));
    code.push_back(kReturn);                            // 11

    FixtureMethod m;
    m.name      = "call";
    m.desc      = "()Ljava/lang/String;";
    m.access    = 0x0009;
    m.maxStack  = 1;
    m.maxLocals = 1;
    m.code      = std::move(code);
    m.exc       = {ExcSpec{0, 3, 6, realCp}};
    owner.addMethod(m);
    addClassResource(container, "pkg/Owner.class", owner.assemble());

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());

    DecodedMethod d;
    ASSERT_TRUE(decodeMethod(container, "pkg/Owner.class", "call", d));
    ASSERT_EQ(d.catchNames.size(), 1u);
    EXPECT_EQ(d.catchNames[0], "java/lang/Exception");
    EXPECT_EQ(countOp(d, kAthrow), 0u);
    EXPECT_EQ(d.ops.size(), 7u);
}

/// 版本戳 45.3 但带成员 → 不是假异常类（判据三重条件缺一不可）。
TEST(MjPassDashoControlFlowTest, RejectsGeneratedVersionWithMembers)
{
    mjbase::model::ResourceContainer container;

    FixtureBuilder notFake("pkg/NotFake");
    notFake.setSuper("java/lang/RuntimeException");
    notFake.addField(FixtureField{"code", "I", 0x0001, {}});
    addClassResource(container, "pkg/NotFake.class", withVersion45_3(notFake.assemble()));
    addClassResource(container, "pkg/Owner.class", buildInjectedOwner("pkg/Owner", "pkg/NotFake"));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());

    DecodedMethod d;
    ASSERT_TRUE(decodeMethod(container, "pkg/Owner.class", "call", d));
    ASSERT_EQ(d.catchNames.size(), 1u);
    EXPECT_EQ(d.catchNames[0], "pkg/NotFake");
    EXPECT_TRUE(container.hasResource("pkg/NotFake.class"));
}

/// 假异常类被指令显式引用（new）→ 异常表项照摘，类不能删。
TEST(MjPassDashoControlFlowTest, KeepsFakeClassReferencedByCode)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Fake.class", buildFakeExceptionClass("pkg/Fake"));

    FixtureBuilder owner("pkg/Owner");
    const ca::u16  fakeCp = owner.cls("pkg/Fake");
    const ca::u16  text   = owner.strConst("hi");

    Bytes call;
    emitGoto(call, 8);
    call.push_back(kAthrow);
    call.push_back(kNop);
    call.push_back(kNop);
    call.push_back(kAthrow);
    call.push_back(kAthrow);
    call.push_back(kLdc);
    call.push_back(static_cast<ca::u8>(text));
    call.push_back(kAreturn);
    call.push_back(kNop);
    call.push_back(kNop);
    call.push_back(kAthrow);
    call.push_back(kAthrow);
    call.push_back(kAstore0);
    call.push_back(kReturn);

    FixtureMethod m;
    m.name      = "call";
    m.desc      = "()Ljava/lang/String;";
    m.access    = 0x0009;
    m.maxStack  = 1;
    m.maxLocals = 1;
    m.code      = std::move(call);
    m.exc       = {ExcSpec{8, 11, 15, fakeCp}};
    owner.addMethod(m);

    FixtureMethod maker;
    maker.name      = "maker";
    maker.desc      = "()V";
    maker.access    = 0x0009;
    maker.maxStack  = 1;
    maker.maxLocals = 0;
    maker.code      = {kNew, static_cast<ca::u8>(fakeCp >> 8), static_cast<ca::u8>(fakeCp), kPop,
                       kReturn};
    owner.addMethod(maker);

    addClassResource(container, "pkg/Owner.class", owner.assemble());

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    DecodedMethod d;
    ASSERT_TRUE(decodeMethod(container, "pkg/Owner.class", "call", d));
    EXPECT_TRUE(d.catchNames.empty());
    // 指令里还有 new Fake → 类必须留着。
    EXPECT_TRUE(container.hasResource("pkg/Fake.class"));
}
