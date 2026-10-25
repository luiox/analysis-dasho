/// DashoStringDecryptPass 单测：夹具手工拼 DashO 的**调用点形态**（外层密钥
/// 算术 + 不透明谓词 + 双支 + 解密调用），断言两条改写策略的分工：
///
///   - **整段折叠**（方法无异常表）：区间塌成一条 `ldc 明文`，死支诱饵一并消失。
///   - **原地改写**（方法有异常表）：只换密文常量 + 把解密调用换成收栈序列，
///     指令条数与栈形不变，死支诱饵留在原地。
///
/// 解密器本身一律是「返回常量」的桩：本 pass 的职责是调用点折叠，解密算法的
/// 求值属于解释器；算法正确性由语料回归（run-lab 26 个 jar + JVM 校验器）覆盖。
#include <gtest/gtest.h>

#include <cstdlib>
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

#include "dasho/string_decrypt_pass.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

// ── opcode 字面量（避免测试读起来全是魔数）──
constexpr ca::u8 kBipush       = 0x10;
constexpr ca::u8 kLdc          = 0x12;
constexpr ca::u8 kIconst0      = 0x03;
constexpr ca::u8 kIconst4      = 0x07;
constexpr ca::u8 kDup          = 0x59;
constexpr ca::u8 kSwap         = 0x5F;
constexpr ca::u8 kImul         = 0x68;
constexpr ca::u8 kIrem         = 0x70;
constexpr ca::u8 kPop          = 0x57;
constexpr ca::u8 kNop          = 0x00;
constexpr ca::u8 kIfeq         = 0x99;
constexpr ca::u8 kIfne         = 0x9A;
constexpr ca::u8 kGoto         = 0xA7;
constexpr ca::u8 kAreturn      = 0xB0;
constexpr ca::u8 kInvokestatic = 0xB8;
constexpr ca::u8 kGetstatic    = 0xB2;

void put16(Bytes& out, ca::u16 v)
{
    out.push_back(static_cast<ca::u8>(v >> 8));
    out.push_back(static_cast<ca::u8>(v));
}

void emitLdc(Bytes& out, ca::u16 cp)
{
    out.push_back(kLdc);
    out.push_back(static_cast<ca::u8>(cp));
}

void emitInvokeStatic(Bytes& out, ca::u16 cp)
{
    out.push_back(kInvokestatic);
    put16(out, cp);
}

void emitBranch(Bytes& out, ca::u8 op, ca::i32 offset)
{
    out.push_back(op);
    put16(out, static_cast<ca::u16>(offset));
}

/// 返回常量的解密器：`ldc <result>; areturn`。
Bytes buildConstDecryptor(const char* className, const char* desc, const char* result,
                          const char* methodName = "a")
{
    FixtureBuilder builder(className);
    FixtureMethod  m;
    m.name      = methodName;
    m.desc      = desc;
    m.access    = 0x0009;   // public static
    m.maxStack  = 1;
    m.maxLocals = 2;
    m.code      = {kLdc, static_cast<ca::u8>(builder.strConst(result)), kAreturn};
    builder.addMethod(m);
    return builder.assemble();
}

/// 不纯解密器（读静态字段）：纯度判定应拒绝它作为锚点。
Bytes buildImpureDecryptor(const char* className, const char* desc)
{
    FixtureBuilder builder(className);
    const ca::u16  cnt = builder.fieldRef(className, "CNT", "I");
    const ca::u16  out = builder.strConst("Hello");

    FixtureMethod m;
    m.name      = "a";
    m.desc      = desc;
    m.access    = 0x0009;
    m.maxStack  = 1;
    m.maxLocals = 2;
    m.code      = {kGetstatic, static_cast<ca::u8>(cnt >> 8), static_cast<ca::u8>(cnt),
                   kPop, kLdc, static_cast<ca::u8>(out), kAreturn};
    builder.addMethod(m);
    return builder.assemble();
}

/// 假内层解密器 `(IILjava/lang/String;)Ljava/lang/String;` → 常量 "fake"。
Bytes buildFakeDecryptor(const char* className)
{
    return buildConstDecryptor(className, "(IILjava/lang/String;)Ljava/lang/String;", "fake",
                               "b");
}

/// DashO 双支模板调用点（无异常表）。@p ifnePolarity 选择谓词极性：
///   false（`ifeq`）：**落空支是诱饵**，跳转支是密文；
///   true （`ifne`）：落空支是密文，跳转支是诱饵。
///
/// 谓词恒真（`(5*4) % 5 == 0`），两支栈形一致（各压 1 个 String）。
Bytes buildTemplateCaller(const char* className, const char* decryptor,
                          const char* cipher, const char* decoy, bool ifnePolarity,
                          bool withExceptionTable)
{
    FixtureBuilder builder(className);
    const ca::u16  dRef   = builder.methodRef(decryptor, "a",
                                             "(IILjava/lang/String;)Ljava/lang/String;");
    const ca::u16  fRef   = builder.methodRef("pkg/Fake", "b",
                                             "(IILjava/lang/String;)Ljava/lang/String;");
    const ca::u16  cCph   = builder.strConst(cipher);
    const ca::u16  cDec   = builder.strConst(decoy);
    const ca::u16  cHdl   = builder.strConst("handler");

    Bytes code;
    // 外层密钥算术：247、72（常量算术，与随机源无关）。
    code.push_back(kBipush); code.push_back(19);
    code.push_back(kBipush); code.push_back(13);
    code.push_back(kImul);
    code.push_back(kBipush); code.push_back(72);
    // 不透明谓词：(5*4) % 5 == 0。
    code.push_back(kBipush); code.push_back(5);
    code.push_back(kDup);
    code.push_back(kIconst4);
    code.push_back(kImul);
    code.push_back(kSwap);
    code.push_back(kIrem);
    // 14: 分支
    const ca::usize branchAt = code.size();
    code.push_back(ifnePolarity ? kIfne : kIfeq);
    code.push_back(0); code.push_back(0);   // 占位，稍后回填

    // 分支目标（谓词走向的那一支）与合流点（两条支都到锚点前）是**两个**
    // 不同位置：`goto` 必须跳到合流点，跳到 `ldc 密文` 会变成自环。
    ca::i32 branchTarget = 0;
    ca::usize gotoAt = 0;
    if (!ifnePolarity) {
        // ifeq：落空支 = 诱饵，跳转支 = 密文。
        code.push_back(kBipush); code.push_back(105);
        code.push_back(kBipush); code.push_back(9);
        emitLdc(code, cDec);
        emitInvokeStatic(code, fRef);
        gotoAt = code.size();
        code.push_back(kGoto); code.push_back(0); code.push_back(0);
        branchTarget = static_cast<ca::i32>(code.size());
        emitLdc(code, cCph);
    }
    else {
        // ifne：落空支 = 密文 + goto 合流，跳转支 = 诱饵（自然落进合流点）。
        emitLdc(code, cCph);
        gotoAt = code.size();
        code.push_back(kGoto); code.push_back(0); code.push_back(0);
        branchTarget = static_cast<ca::i32>(code.size());
        code.push_back(kBipush); code.push_back(105);
        code.push_back(kBipush); code.push_back(9);
        emitLdc(code, cDec);
        emitInvokeStatic(code, fRef);
    }
    // JVMS 6.5：分支偏移相对**分支指令自身的 pc**（不是下一条指令）。
    const ca::usize joinPc = code.size();
    const auto patch = [&](ca::usize at, ca::i32 target) {
        const ca::i32 off = target - static_cast<ca::i32>(at);
        code[at + 1] = static_cast<ca::u8>(static_cast<ca::u16>(off) >> 8);
        code[at + 2] = static_cast<ca::u8>(off);
    };
    patch(branchAt, branchTarget);
    patch(gotoAt, static_cast<ca::i32>(joinPc));

    emitInvokeStatic(code, dRef);
    code.push_back(kAreturn);

    FixtureMethod call;
    call.name      = "call";
    call.desc      = "()Ljava/lang/String;";
    call.access    = 0x0009;
    call.maxStack  = 6;
    call.maxLocals = 0;
    if (withExceptionTable) {
        const ca::u16 handlerPc = static_cast<ca::u16>(code.size());
        code.push_back(kPop);
        emitLdc(code, cHdl);
        code.push_back(kAreturn);
        call.exc.push_back(ExcSpec{0, handlerPc, handlerPc, 0});
    }
    call.code = std::move(code);
    builder.addMethod(call);
    return builder.assemble();
}

/// 简单调用点：`ldc cipher; [bipush 7]; invokestatic a; areturn`，可选异常表。
/// @p desc 决定实参形态（是否多一个 int，以及 int 在密文前还是后）。
Bytes buildSimpleCaller(const char* className, const char* decryptor, const char* desc,
                        const char* cipher, bool intArg, bool intFirst,
                        bool withExceptionTable)
{
    FixtureBuilder builder(className);
    const ca::u16  dRef = builder.methodRef(decryptor, "a", desc);
    const ca::u16  cCph = builder.strConst(cipher);
    const ca::u16  cHdl = builder.strConst("handler");

    Bytes code;
    if (intArg && intFirst) {
        code.push_back(kBipush); code.push_back(7);
        emitLdc(code, cCph);
    }
    else {
        emitLdc(code, cCph);
        if (intArg) {
            code.push_back(kBipush); code.push_back(7);
        }
    }
    emitInvokeStatic(code, dRef);
    code.push_back(kAreturn);

    FixtureMethod call;
    call.name      = "call";
    call.desc      = "()Ljava/lang/String;";
    call.access    = 0x0009;
    call.maxStack  = 4;
    call.maxLocals = 0;
    if (withExceptionTable) {
        const ca::u16 handlerPc = static_cast<ca::u16>(code.size());
        code.push_back(kPop);
        emitLdc(code, cHdl);
        code.push_back(kAreturn);
        call.exc.push_back(ExcSpec{0, handlerPc, handlerPc, 0});
    }
    call.code = std::move(code);
    builder.addMethod(call);
    return builder.assemble();
}

/// 嵌套调用点：`ldc cipher; invokestatic Inner; invokestatic Outer; areturn`。
/// 内层是「假解密」，外层是真正的锚点——最外层优先必须让两者一次折叠。
Bytes buildNestedCaller(const char* className, const char* inner, const char* outer)
{
    FixtureBuilder builder(className);
    const ca::u16  iRef = builder.methodRef(inner, "b",
                                            "(IILjava/lang/String;)Ljava/lang/String;");
    const ca::u16  oRef = builder.methodRef(outer, "a", "(Ljava/lang/String;)Ljava/lang/String;");
    const ca::u16  cCph = builder.strConst("cipher");

    FixtureMethod call;
    call.name      = "call";
    call.desc      = "()Ljava/lang/String;";
    call.access    = 0x0009;
    call.maxStack  = 4;
    call.maxLocals = 0;
    call.code      = {
        kBipush, 9,
        kBipush, 3,
        kLdc, static_cast<ca::u8>(cCph),
        kInvokestatic, static_cast<ca::u8>(iRef >> 8), static_cast<ca::u8>(iRef),
        kInvokestatic, static_cast<ca::u8>(oRef >> 8), static_cast<ca::u8>(oRef),
        kAreturn,
    };
    builder.addMethod(call);
    return builder.assemble();
}

/// 反汇编出的指令序列（op + ldc 常量），供断言使用。
struct Decoded
{
    std::vector<ca::u8>        ops;
    std::vector<std::string>   ldcStrings;   ///< 与 ops 等长；非 ldc 处为空
};

bool decodeCall(mjbase::model::ResourceContainer& container, const char* uri, Decoded& out)
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
        if (file->nameOf(m) == "call") target = &m;
    }
    if (target == nullptr) return false;
    auto decoded = file->decodeCode(*target);
    if (decoded.is_err()) return false;
    const jbc::core::CodeAttr* code = std::move(decoded).unwrap();
    if (code == nullptr) return false;

    auto domRes = jbc::ir::CodeDom::importFrom(jbc::core::ByteView(code->code));
    if (domRes.is_err()) return false;
    jbc::ir::CodeDom d = std::move(domRes).unwrap();

    for (const jbc::ir::InsnId id : d.order()) {
        const jbc::ir::Insn* in = d.peek(id);
        if (in == nullptr) return false;
        out.ops.push_back(static_cast<ca::u8>(in->op));
        std::string text;
        if (in->op == jbc::core::Op::LDC || in->op == jbc::core::Op::LDC_W) {
            if (file->cp.valid(in->cp)) {
                const auto& entry = file->cp.at(in->cp);
                if (entry.tag == jbc::core::CpTag::String) {
                    text.assign(file->utf8(entry.a));
                }
            }
        }
        out.ldcStrings.push_back(std::move(text));
    }
    return true;
}

mjbase::spi::PreservedAnalyses runPass(mjbase::model::ResourceContainer& container)
{
    dasho::DashoStringDecryptPass       pass;
    ca::str::Utf8StringArena            arena;
    mjbase::spi::GlobalAnalysisManager  gam(arena);
    return pass.run(container, gam);
}

ca::usize countOp(const Decoded& d, ca::u8 op)
{
    ca::usize n = 0;
    for (const ca::u8 o : d.ops) {
        if (o == op) ++n;
    }
    return n;
}

/// 第一个非空的 ldc 明文。
std::string firstPlain(const Decoded& d)
{
    for (const std::string& s : d.ldcStrings) {
        if (!s.empty()) return s;
    }
    return {};
}

}   // namespace

// ── 用例一：整段折叠（无异常表，ifeq 极性）──
// 谓词恒真 → 跳转支是密文；区间塌成一条 ldc "Hello"，死支诱饵消失。
TEST(MjPassDashoStringDecryptTest, FoldsWholeRegionWhenNoExceptionTable)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Dec.class",
                     buildConstDecryptor("pkg/Dec", "(IILjava/lang/String;)Ljava/lang/String;",
                                         "Hello"));
    addClassResource(container, "pkg/Fake.class", buildFakeDecryptor("pkg/Fake"));
    addClassResource(container, "pkg/Caller.class",
                     buildTemplateCaller("pkg/Caller", "pkg/Dec", "cipher", "decoy",
                                         /*ifnePolarity=*/false, /*withExceptionTable=*/false));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    // 塌缩为 ldc "Hello" + areturn：谓词、两支、假解密全部消失。
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ops[0], kLdc);
    EXPECT_EQ(d.ldcStrings[0], "Hello");
    EXPECT_EQ(d.ops[1], kAreturn);
    EXPECT_EQ(countOp(d, kInvokestatic), 0u);
}

// ── 用例二：ifne 极性同样折叠 ──
// 落空支是密文、跳转支是诱饵（以 goto 收尾）——「合法出口」路径必须成立。
TEST(MjPassDashoStringDecryptTest, FoldsWhenFallthroughCarriesCipher)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Dec.class",
                     buildConstDecryptor("pkg/Dec", "(IILjava/lang/String;)Ljava/lang/String;",
                                         "Hello"));
    addClassResource(container, "pkg/Fake.class", buildFakeDecryptor("pkg/Fake"));
    addClassResource(container, "pkg/Caller.class",
                     buildTemplateCaller("pkg/Caller", "pkg/Dec", "cipher", "decoy",
                                         /*ifnePolarity=*/true, /*withExceptionTable=*/false));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ldcStrings[0], "Hello");
}

// ── 用例三：带异常表 → 原地改写，诱饵保留 ──
// 指令条数不变（密文 ldc → 明文 ldc；invokestatic → swap;pop;swap;pop），
// 谓词与死支诱饵仍在原地：原帧逐条仍然成立，不需要也不可能安全重算。
TEST(MjPassDashoStringDecryptTest, RewritesInPlaceWhenExceptionTablePresent)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Dec.class",
                     buildConstDecryptor("pkg/Dec", "(IILjava/lang/String;)Ljava/lang/String;",
                                         "Hello"));
    addClassResource(container, "pkg/Fake.class", buildFakeDecryptor("pkg/Fake"));
    addClassResource(container, "pkg/Caller.class",
                     buildTemplateCaller("pkg/Caller", "pkg/Dec", "cipher", "decoy",
                                         /*ifnePolarity=*/false, /*withExceptionTable=*/true));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    // 谓词仍在（死支诱饵未被擦除）——这是与整段折叠的分水岭。
    EXPECT_EQ(countOp(d, kIfeq), 1u);
    EXPECT_EQ(countOp(d, kIrem), 1u);
    // 两个解密调用（外层真解密 + 死支里的假解密）都换成了收栈序列：原地
    // 改写不区分活支死支，凡能求值的站点一律换掉——留下的死支里若还有
    // 可折叠的调用，后续轮次同样会折（净栈效应不变，纯方法无副作用）。
    EXPECT_EQ(countOp(d, kInvokestatic), 0u);
    EXPECT_GE(countOp(d, kSwap), 2u);
    // 密文 ldc 变成明文，且只出现一次。
    ca::usize plainHits = 0;
    for (const std::string& s : d.ldcStrings) {
        if (s == "Hello") ++plainHits;
    }
    EXPECT_EQ(plainHits, 1u);
    // 密文本身不再被引用。
    for (const std::string& s : d.ldcStrings) EXPECT_NE(s, "cipher");
}

// ── 用例四：收栈序列随实参形态变化 ──
// `(String,int)`  → pop        （密文在下方，弹掉密钥）
// `(int,String)`  → swap;pop   （密文在顶，掏掉下方密钥）
// `(String)`      → nop        （调用本身就是恒等）
TEST(MjPassDashoStringDecryptTest, PopsArgumentsAboveCipherForStringInt)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Dec.class",
                     buildConstDecryptor("pkg/Dec", "(Ljava/lang/String;I)Ljava/lang/String;",
                                         "Hello"));
    addClassResource(container, "pkg/Caller.class",
                     buildSimpleCaller("pkg/Caller", "pkg/Dec", "(Ljava/lang/String;I)Ljava/lang/String;",
                                       "cipher", /*intArg=*/true, /*intFirst=*/false,
                                       /*withExceptionTable=*/true));

    ASSERT_TRUE(runPass(container).changed());
    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    EXPECT_EQ(countOp(d, kPop), 2u);   // 收栈 pop + handler 的 pop
    EXPECT_EQ(countOp(d, kSwap), 0u);
    EXPECT_EQ(countOp(d, kInvokestatic), 0u);
}

TEST(MjPassDashoStringDecryptTest, SwapsArgumentBelowCipherForIntString)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Dec.class",
                     buildConstDecryptor("pkg/Dec", "(ILjava/lang/String;)Ljava/lang/String;",
                                         "Hello"));
    addClassResource(container, "pkg/Caller.class",
                     buildSimpleCaller("pkg/Caller", "pkg/Dec", "(ILjava/lang/String;)Ljava/lang/String;",
                                       "cipher", /*intArg=*/true, /*intFirst=*/true,
                                       /*withExceptionTable=*/true));

    ASSERT_TRUE(runPass(container).changed());
    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    EXPECT_EQ(countOp(d, kSwap), 1u);
    EXPECT_EQ(countOp(d, kPop), 2u);   // swap;pop 的 pop + handler 的 pop
    EXPECT_EQ(countOp(d, kInvokestatic), 0u);
}

TEST(MjPassDashoStringDecryptTest, ReplacesSingleArgCallWithNop)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Dec.class",
                     buildConstDecryptor("pkg/Dec", "(Ljava/lang/String;)Ljava/lang/String;",
                                         "Hello"));
    addClassResource(container, "pkg/Caller.class",
                     buildSimpleCaller("pkg/Caller", "pkg/Dec", "(Ljava/lang/String;)Ljava/lang/String;",
                                       "cipher", /*intArg=*/false, /*intFirst=*/false,
                                       /*withExceptionTable=*/true));

    ASSERT_TRUE(runPass(container).changed());
    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    EXPECT_EQ(countOp(d, kNop), 1u);
    EXPECT_EQ(countOp(d, kSwap), 0u);
    EXPECT_EQ(countOp(d, kInvokestatic), 0u);
}

// ── 用例五：自定义加密 `(String,String)`（用户解密器，无谓词）──
TEST(MjPassDashoStringDecryptTest, FoldsCustomEncryptionTwoStringArgs)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Xor.class",
                     buildConstDecryptor("pkg/Xor", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                         "Secret"));
    addClassResource(container, "pkg/Caller.class",
                     buildSimpleCaller("pkg/Caller", "pkg/Xor",
                                       "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                       "cipher", /*intArg=*/true, /*intFirst=*/true,
                                       /*withExceptionTable=*/false));

    ASSERT_TRUE(runPass(container).changed());
    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    // 整段折叠：密钥 ldc + 密文 ldc + 调用 → 一条 ldc "Secret"。
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ldcStrings[0], "Secret");
}

// ── 用例六：嵌套调用一次折叠（最外层优先）──
// 先折内层会把实参压成 1 个值，外层区域仍按原始实参个数改写 → 两条路径栈深
// 不一致。倒序扫描锚点让外层先塌缩，内层作为区域一部分被吸收。
TEST(MjPassDashoStringDecryptTest, FoldsNestedCallInSinglePass)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Inner.class", buildFakeDecryptor("pkg/Inner"));
    addClassResource(container, "pkg/Outer.class",
                     buildConstDecryptor("pkg/Outer", "(Ljava/lang/String;)Ljava/lang/String;",
                                         "OuterPlain"));
    addClassResource(container, "pkg/Caller.class",
                     buildNestedCaller("pkg/Caller", "pkg/Inner", "pkg/Outer"));

    ASSERT_TRUE(runPass(container).changed());
    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ldcStrings[0], "OuterPlain");
    EXPECT_EQ(countOp(d, kInvokestatic), 0u);
}

// ── 用例七：不纯解密器（读静态字段）不是锚点 ──
TEST(MjPassDashoStringDecryptTest, SkipsImpureDecryptor)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Dec.class",
                     buildImpureDecryptor("pkg/Dec", "(Ljava/lang/String;)Ljava/lang/String;"));
    addClassResource(container, "pkg/Caller.class",
                     buildSimpleCaller("pkg/Caller", "pkg/Dec", "(Ljava/lang/String;)Ljava/lang/String;",
                                       "cipher", false, false, /*withExceptionTable=*/false));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decodeCall(container, "pkg/Caller.class", d));
    EXPECT_EQ(countOp(d, kInvokestatic), 1u);   // 调用点原样保留
}

// ── 用例八：解密器不在 jar 内（外部依赖）→ 不猜，跳过 ──
TEST(MjPassDashoStringDecryptTest, SkipsDecryptorOutsideJar)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Caller.class",
                     buildSimpleCaller("pkg/Caller", "pkg/Absent",
                                       "(Ljava/lang/String;)Ljava/lang/String;",
                                       "cipher", false, false, /*withExceptionTable=*/false));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());
}

// ── 用例九：非法明文（C1 控制符）不写回 ──
// 错误求值的产物常带控制符；明文合法性是最后一道闸。
TEST(MjPassDashoStringDecryptTest, SkipsIllegalPlaintext)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Dec.class",
                     buildConstDecryptor("pkg/Dec", "(Ljava/lang/String;)Ljava/lang/String;",
                                         "\x7F"));
    addClassResource(container, "pkg/Caller.class",
                     buildSimpleCaller("pkg/Caller", "pkg/Dec", "(Ljava/lang/String;)Ljava/lang/String;",
                                       "cipher", false, false, /*withExceptionTable=*/false));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());
}
