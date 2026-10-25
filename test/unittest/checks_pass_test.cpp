/// DashoChecksPass 单测：夹具手工拼 DashO `injection + *Check` 的**注入产物**
/// （噪声谓词 + 反射三连 + 响应 + 整段 try/catch），断言四类判据的边界：
///
///   - **正例**：段被整段删除，方法体回到原逻辑；注入 helper 一并清扫。
///   - **缺噪声谓词**：只剩反射三连 → 不动。真实业务代码里 `Class.forName` +
///     `getMethod` + `invoke` 很常见，单靠三连会误删活代码。
///   - **缺响应**：只有噪声谓词与反射、没有 exit/halt/sleep/athrow → 不动。
///     DashO 自己也会把这种配置判成 unused，所以"有响应"是必要条件。
///   - **区间有外部入口**：hammock 判据失败 → 不动。删掉会让那条边悬垂。
///   - **helper 仍被活代码引用**：不删。
///
/// 语料（`SnakeGame-checks-debug.jar` / 四重混淆的 `SnakeGame-all.jar`）
/// 的形态与还原结果由语料回归覆盖。
#include <gtest/gtest.h>

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

#include "dasho/checks_pass.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u8 kNop           = 0x00;
constexpr ca::u8 kAconstNull    = 0x01;
constexpr ca::u8 kIconst0       = 0x03;
constexpr ca::u8 kIconst1       = 0x04;
constexpr ca::u8 kIconst4       = 0x07;
constexpr ca::u8 kBipush        = 0x10;
constexpr ca::u8 kLdc           = 0x12;
constexpr ca::u8 kDup           = 0x59;
constexpr ca::u8 kSwap          = 0x5f;
constexpr ca::u8 kImul          = 0x68;
constexpr ca::u8 kIrem          = 0x70;
constexpr ca::u8 kIfeq          = 0x99;
constexpr ca::u8 kGoto          = 0xa7;
constexpr ca::u8 kAreturn       = 0xb0;
constexpr ca::u8 kIreturn       = 0xac;
constexpr ca::u8 kAthrow        = 0xbf;
constexpr ca::u8 kAstore1       = 0x4c;
constexpr ca::u8 kAstore2       = 0x4d;
constexpr ca::u8 kAload1        = 0x2b;
constexpr ca::u8 kAload2        = 0x2c;
constexpr ca::u8 kPop          = 0x57;
constexpr ca::u8 kAnewarray     = 0xbd;
constexpr ca::u8 kInvokestatic  = 0xb8;
constexpr ca::u8 kInvokevirtual = 0xb6;

/// 夹具变体。所有变体保持**逐字节相同的布局**，只有判据点不同——这样断言
/// 的 pc 与区间边界在各例之间可直接对照。
enum class Variant
{
    Full,             ///< 正例：噪声谓词 + 反射三连 + exit 响应
    NoNoisePredicate, ///< 谓词换成普通常量分支（不是 (seed*k)%seed）
    NoResponse,       ///< 响应换成 nop 填充
    ExternalEntry,    ///< 方法尾加一条跳回区内的边（hammock 失败）
    EndInjection,     ///< `where="End"` 形态：原逻辑在区间之前，落下进入区间
};

void emitU16(Bytes& out, ca::i32 v)
{
    out.push_back(static_cast<ca::u8>(static_cast<ca::u16>(v) >> 8));
    out.push_back(static_cast<ca::u8>(v));
}

void emitCall(Bytes& out, ca::u8 op, ca::u16 idx)
{
    out.push_back(op);
    emitU16(out, idx);
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

/// 注入 helper 类：`Owner$1` 形态（无字段 / 无 <init> / 全 static / 一个返回
/// String 的方法 + 一个 `()I` 种子生成器）。
Bytes buildHelperClass(const char* name)
{
    FixtureBuilder builder(name);

    FixtureMethod seed;
    seed.name      = "seed";
    seed.desc      = "()I";
    seed.access    = 0x0009;
    seed.maxStack  = 1;
    seed.maxLocals = 0;
    seed.code      = {kIconst1, kIreturn};
    builder.addMethod(seed);

    FixtureMethod dec;
    dec.name      = "dec";
    dec.desc      = "(Ljava/lang/String;)Ljava/lang/String;";
    dec.access    = 0x0009;
    dec.maxStack  = 1;
    dec.maxLocals = 1;
    dec.code      = {0x2a, kAreturn};   // aload_0; areturn
    builder.addMethod(dec);

    return builder.assemble();
}

/// 宿主类：`run()Ljava/lang/String;` 里带整段检查注入。
///
/// 布局（各变体一致）：
/// ```
///   0  invokestatic Helper.seed()I      ; 噪声谓词起点
///   3  dup / iconst_4 / imul / swap / irem
///   8  ifeq 19
///  11  ldc cA ; invokestatic dec ; goto 22
///  19  ldc cB
///  21  nop
///  22  invokestatic Class.forName
///  25  astore_1 ; aload_1 ; ldc "poll" ; iconst_0 ; anewarray Class
///  33  invokevirtual Class.getMethod
///  36  astore_2 ; aload_2 ; aconst_null ; iconst_0 ; anewarray Object
///  43  invokevirtual Method.invoke
///  46  pop
///  47  goto 64                     ; 跳出 try
///  50  nop x5                      ; 死填充
///  55  pop                         ; handler 入口
///  56  bipush -5 ; invokestatic System.exit
///  61  goto 64
///  64  ldc "ok" ; areturn          ; 原逻辑
/// ```
Bytes buildHost(const char* name, Variant variant, bool keepHelperLive = false)
{
    FixtureBuilder builder(name);
    const ca::u16  seed    = builder.methodRef("pkg/Helper", "seed", "()I");
    const ca::u16  dec     = builder.methodRef("pkg/Helper", "dec",
                                               "(Ljava/lang/String;)Ljava/lang/String;");
    const ca::u16  forName = builder.methodRef("java/lang/Class", "forName",
                                               "(Ljava/lang/String;)Ljava/lang/Class;");
    const ca::u16  getMethod = builder.methodRef("java/lang/Class", "getMethod",
                                                 "(Ljava/lang/String;[Ljava/lang/Class;)"
                                                 "Ljava/lang/reflect/Method;");
    const ca::u16  invoke    = builder.methodRef("java/lang/reflect/Method", "invoke",
                                                 "(Ljava/lang/Object;[Ljava/lang/Object;)"
                                                 "Ljava/lang/Object;");
    const ca::u16  exit      = builder.methodRef("java/lang/System", "exit", "(I)V");
    const ca::u16  classArr  = builder.cls("java/lang/Class");
    const ca::u16  objectArr = builder.cls("java/lang/Object");
    const ca::u16  cipherA   = builder.strConst("cipherA");
    const ca::u16  cipherB   = builder.strConst("cipherB");
    const ca::u16  pollName  = builder.strConst("poll");
    const ca::u16  okText    = builder.strConst("ok");
    const ca::u16  throwable = builder.cls("java/lang/Exception");

    Bytes code;
    // `where="End"` 形态：DashO 把原表达式的值溢写到局部变量、注入段接在后面、
    // 检查跑完再读回来。这里用 3 字节的 `ldc "ok"; astore_1` 当序言，于是区间
    // 起点是 3 而不是 0，且必然有一条从 pc 2 落下进入区间的**入口边**。
    // 摘掉注入段后 `astore_1` 与 `aload_1` 相邻，溢写对折叠应当把它一并收掉。
    const ca::i32 sh = (variant == Variant::EndInjection) ? 3 : 0;
    if (sh != 0) {
        emitLdc(code, okText);
        code.push_back(kAstore1);
    }

    if (variant == Variant::NoNoisePredicate) {
        // 同样的 8 字节前缀，但不是一个 `()I` 调用：判据不该认。
        code.push_back(kIconst1);
        for (int i = 0; i < 7; ++i) code.push_back(kNop);
    } else {
        emitCall(code, kInvokestatic, seed);   // 0
        code.push_back(kDup);                  // 3
        code.push_back(kIconst4);              // 4
        code.push_back(kImul);                 // 5
        code.push_back(kSwap);                 // 6
        code.push_back(kIrem);                 // 7
    }
    emitBranch(code, kIfeq, 19 + sh);          // 8
    emitLdc(code, cipherA);                    // 11
    emitCall(code, kInvokestatic, dec);        // 13
    emitBranch(code, kGoto, 22 + sh);          // 16
    emitLdc(code, cipherB);                    // 19
    code.push_back(kNop);                      // 21
    emitCall(code, kInvokestatic, forName);    // 22
    code.push_back(kAstore1);                  // 25
    code.push_back(kAload1);                   // 26
    emitLdc(code, pollName);                   // 27
    code.push_back(kIconst0);                  // 29
    emitCall(code, kAnewarray, classArr);      // 30
    emitCall(code, kInvokevirtual, getMethod); // 33
    code.push_back(kAstore2);                  // 36
    code.push_back(kAload2);                   // 37
    code.push_back(kAconstNull);               // 38
    code.push_back(kIconst0);                  // 39
    emitCall(code, kAnewarray, objectArr);     // 40
    emitCall(code, kInvokevirtual, invoke);    // 43
    code.push_back(kPop);                      // 46
    emitBranch(code, kGoto, 64 + sh);          // 47
    for (int i = 0; i < 5; ++i) code.push_back(kNop);   // 50..54
    code.push_back(kPop);                      // 55  handler 入口
    if (variant == Variant::NoResponse) {
        for (int i = 0; i < 5; ++i) code.push_back(kNop);   // 56..60
    } else {
        code.push_back(kBipush);               // 56
        code.push_back(0xfb);                  // -5
        emitCall(code, kInvokestatic, exit);   // 58
    }
    emitBranch(code, kGoto, 64 + sh);          // 61
    if (variant == Variant::EndInjection) {
        code.push_back(kAload1);               // 64+sh  读回溢写的值
        code.push_back(kAreturn);              // 65+sh
    } else {
        emitLdc(code, okText);                 // 64
        code.push_back(kAreturn);              // 66
    }
    if (variant == Variant::ExternalEntry) {
        emitBranch(code, kGoto, 22);           // 67 外部入口 -> 区间内部
    }

    FixtureMethod run;
    run.name      = "run";
    run.desc      = "()Ljava/lang/String;";
    run.access    = 0x0009;
    run.maxStack  = 4;
    run.maxLocals = 3;
    run.code      = std::move(code);
    run.exc       = {{static_cast<ca::u16>(sh), static_cast<ca::u16>(55 + sh),
                      static_cast<ca::u16>(55 + sh), throwable}};
    builder.addMethod(run);

    if (keepHelperLive) {
        FixtureMethod other;
        other.name      = "other";
        other.desc      = "()Ljava/lang/String;";
        other.access    = 0x0009;
        other.maxStack  = 1;
        other.maxLocals = 0;
        other.code      = {};
        emitLdc(other.code, cipherA);
        emitCall(other.code, kInvokestatic, dec);
        other.code.push_back(kAreturn);
        builder.addMethod(other);
    }

    return builder.assemble();
}

mjbase::spi::PreservedAnalyses runPass(mjbase::model::ResourceContainer& container)
{
    dasho::DashoChecksPass      pass;
    ca::str::Utf8StringArena    arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    return pass.run(container, gam);
}

/// 反汇编：op 序列 + 异常表条目数。
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

bool hasClass(mjbase::model::ResourceContainer& container, const char* uri)
{
    return container.getResource(uri) != nullptr;
}

}   // namespace

/// 正例：整段删除，方法体只剩原逻辑；异常表清空；注入 helper 一并清扫。
TEST(MjPassDashoChecksTest, StripsInjectedSegmentAndSweepsHelper)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Helper.class", buildHelperClass("pkg/Helper"));
    addClassResource(container, "pkg/Host.class", buildHost("pkg/Host", Variant::Full));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    EXPECT_EQ(d.catchEntries, 0u);
    // 只剩真代码：ldc "ok" + areturn。
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ops[0], kLdc);
    EXPECT_EQ(d.ops[1], kAreturn);

    // 调用点全没了，helper 在活指令里再无引用 -> 删掉。
    EXPECT_FALSE(hasClass(container, "pkg/Helper.class"));
}

/// 缺噪声谓词：只有反射三连 + 响应 -> 不动。这是"别误删真实反射代码"的底线。
TEST(MjPassDashoChecksTest, KeepsReflectionWithoutNoisePredicate)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Helper.class", buildHelperClass("pkg/Helper"));
    addClassResource(container, "pkg/Host.class",
                     buildHost("pkg/Host", Variant::NoNoisePredicate));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());
    EXPECT_TRUE(hasClass(container, "pkg/Helper.class"));
}

/// 缺响应：噪声谓词 + 反射三连，但没有 exit/halt/sleep/athrow -> 不动。
TEST(MjPassDashoChecksTest, KeepsNoiseWithoutResponse)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Helper.class", buildHelperClass("pkg/Helper"));
    addClassResource(container, "pkg/Host.class", buildHost("pkg/Host", Variant::NoResponse));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());
}

/// 区间有外部入口，但入口落在**区间内部**（方法尾跳回 pc 22）：hammock 失败。
/// 删掉会让那条边指向已删指令，且语义上等于跳进半截注入段。
TEST(MjPassDashoChecksTest, KeepsRegionWithExternalEntry)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Helper.class", buildHelperClass("pkg/Helper"));
    addClassResource(container, "pkg/Host.class",
                     buildHost("pkg/Host", Variant::ExternalEntry));

    const auto pa = runPass(container);
    EXPECT_FALSE(pa.changed());
}

/// `where="End"` 形态：原逻辑在区间之前，靠**落下**进入区间（f=3 而非 0）。
/// 这条入口边不该挡住剥离——删完之后它自然落到区间右端。
/// 顺带覆盖溢写对折叠：`astore_1` 与 `aload_1` 相邻后应当被一并删掉。
TEST(MjPassDashoChecksTest, StripsSegmentWithFallthroughEntry)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Helper.class", buildHelperClass("pkg/Helper"));
    addClassResource(container, "pkg/Host.class",
                     buildHost("pkg/Host", Variant::EndInjection));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    EXPECT_EQ(d.catchEntries, 0u);
    // 只剩 `ldc "ok" + areturn`：溢写对 `astore_1; aload_1` 也被折叠掉了。
    ASSERT_EQ(d.ops.size(), 2u);
    EXPECT_EQ(d.ops[0], kLdc);
    EXPECT_EQ(d.ops[1], kAreturn);

    EXPECT_FALSE(hasClass(container, "pkg/Helper.class"));
}

/// helper 仍被活代码引用（另一方法还在调它的解密器）-> 保留。
TEST(MjPassDashoChecksTest, KeepsHelperStillReferencedByLiveCode)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "pkg/Helper.class", buildHelperClass("pkg/Helper"));
    addClassResource(container, "pkg/Host.class",
                     buildHost("pkg/Host", Variant::Full, /*keepHelperLive=*/true));

    const auto pa = runPass(container);
    EXPECT_TRUE(pa.changed());

    Decoded d;
    ASSERT_TRUE(decode(container, "pkg/Host.class", "run", d));
    ASSERT_EQ(d.ops.size(), 2u);

    // `other()` 还在调 Helper.dec，所以 helper 必须留着。
    EXPECT_TRUE(hasClass(container, "pkg/Helper.class"));
}
