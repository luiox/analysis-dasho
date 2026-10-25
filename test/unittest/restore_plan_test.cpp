/// restore_plan 单测：mapfile 反演成重映射计划时的三个坑。
///
/// 这三个坑的共同点是**错了也不报错**——规则装进计划后静默不命中，pass 跑完
/// 一个字节没改，日志还显示"装载了 N 条规则"。所以只能靠断言计划内容来钉：
///   1. 方向：mapfile 是 原名→混淆名，计划必须是 混淆名→原名；
///   2. 描述符：源键用混淆空间（`()Ll;`），目标键用原名空间（`()LTuple;`）；
///   3. 同名条目：`<init>`/`serialVersionUID` 这类不进计划。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "dasho/rename_map.hpp"
#include "dasho/restore_plan.hpp"

namespace {

/// 与 run-lab `SnakeGame-renaming-default.map` 同构的最小片段：
/// `Tuple` → `l`，字段 `x:I` → `i`，方法 `getX()I` → `i`；
/// `ThreadsController` → `j`，方法 `getValAleaNotInSnake()LTuple;` → `iJ`。
constexpr const char* kMapText = R"(|Tuple|l|
=<init>=(II)V=<init>=
=getX=()I=i=
=getY=()I=J=
+x+I+i+
+y+I+J+

|ThreadsController|j|
=<init>=(LTuple;)V=<init>=
=getValAleaNotInSnake=()LTuple;=iJ=
+headSnakePos+LTuple;+J+

|Main|Main|
=<init>=()V=<init>=
=main=([Ljava/lang/String;)V=i=
)";

dasho::RenameMap parse(const char* text)
{
    auto parsed = dasho::parseDashoMap(text);
    EXPECT_TRUE(parsed.is_ok());
    return std::move(parsed).unwrap();
}

TEST(RestorePlan, ReversesDirection)
{
    const dasho::RenameMap map = parse(kMapText);

    mjbase::asm_remapper::MemberRemapPlan plan;
    const dasho::RestorePlanStats       stats = dasho::buildRestorePlan(map, plan);

    // 混淆名 → 原名（不是反过来）。
    EXPECT_EQ(plan.mapInternalName("l"), "Tuple");
    EXPECT_EQ(plan.mapInternalName("j"), "ThreadsController");
    // 未改名的类不在计划里，恒等返回。
    EXPECT_EQ(plan.mapInternalName("Main"), "Main");
    EXPECT_EQ(stats.classRules, 2u);
}

TEST(RestorePlan, PreimagesDescriptorInObfuscatedSpace)
{
    const dasho::RenameMap map = parse(kMapText);

    mjbase::asm_remapper::MemberRemapPlan plan;
    dasho::buildRestorePlan(map, plan);

    // 源键必须是混淆空间的描述符：`()LTuple;` → `()Ll;`。
    // 用原名空间的描述符查不到，规则等于没装。
    const auto* hit = plan.findMethod("j", "iJ", "()Ll;");
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(plan.pool().str(hit->newKey.cls), "ThreadsController");
    EXPECT_EQ(plan.pool().str(hit->newKey.name), "getValAleaNotInSnake");
    EXPECT_EQ(plan.pool().str(hit->newKey.desc), "()LTuple;");

    // 原名空间的描述符在源键上必须查不到（反证上一条不是碰巧）。
    EXPECT_EQ(plan.findMethod("j", "iJ", "()LTuple;"), nullptr);
}

TEST(RestorePlan, ReversesFieldsToo)
{
    const dasho::RenameMap map = parse(kMapText);

    mjbase::asm_remapper::MemberRemapPlan plan;
    dasho::buildRestorePlan(map, plan);

    const auto* hit = plan.findField("j", "J", "Ll;");
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(plan.pool().str(hit->newKey.name), "headSnakePos");
    EXPECT_EQ(plan.pool().str(hit->newKey.desc), "LTuple;");

    // 字段与方法是两套规则，不能串。
    EXPECT_EQ(plan.findMethod("j", "J", "Ll;"), nullptr);
}

TEST(RestorePlan, SkipsIdentityEntries)
{
    const dasho::RenameMap map = parse(kMapText);

    mjbase::asm_remapper::MemberRemapPlan plan;
    const dasho::RestorePlanStats       stats = dasho::buildRestorePlan(map, plan);

    // `<init>` ×3 = 3 条同名方法；Main 类本身也不改名。
    EXPECT_EQ(stats.identitySkipped, 3u);
    // getX / getY / getValAleaNotInSnake / main（入口类的类名保留，方法照样改名）
    EXPECT_EQ(stats.methodRules, 4u);
    EXPECT_EQ(stats.fieldRules, 3u);    // x / y / headSnakePos
}

TEST(RestorePlan, ReportsUnmappedOwner)
{
    dasho::RenameMap map = parse(kMapText);
    // 成员挂在一个类表里没有的类上：既不知道混淆名，就无法定位。
    map.members.push_back(dasho::MemberRename{"Ghost", "foo", "()V", "a", true});

    mjbase::asm_remapper::MemberRemapPlan plan;
    const dasho::RestorePlanStats       stats = dasho::buildRestorePlan(map, plan);

    EXPECT_EQ(stats.unmappedOwner, 1u);
}

TEST(RestorePlan, SkipsConflictingClasses)
{
    const dasho::RenameMap map = parse(kMapText);

    dasho::RestorePlanOptions options;
    options.skipObfClasses.insert("j");

    mjbase::asm_remapper::MemberRemapPlan plan;
    const dasho::RestorePlanStats       stats = dasho::buildRestorePlan(map, plan, options);

    EXPECT_EQ(plan.mapInternalName("j"), "j");      // 冲突类不改名
    EXPECT_EQ(plan.mapInternalName("l"), "Tuple");  // 其余照改
    EXPECT_EQ(stats.uriConflict, 1u);
    // 冲突类的成员规则也不装（类名不动，反演目标会撞现有条目）。
    EXPECT_EQ(plan.findMethod("j", "iJ", "()Ll;"), nullptr);
}

TEST(RestorePlan, DetectsUriConflict)
{
    const dasho::RenameMap map = parse(kMapText);

    // `Tuple.class` 还在，且它自己不会被改走（不在任何规则的混淆名一侧）。
    const std::vector<std::string> uris = {"l.class", "j.class", "Tuple.class"};
    const auto conflicts = dasho::findClassUriConflicts(map, uris);
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts.count("l"), 1u);

    // 目标空着 → 无冲突。
    const std::vector<std::string> clean = {"l.class", "j.class"};
    EXPECT_TRUE(dasho::findClassUriConflicts(map, clean).empty());
}

TEST(RewriteDescriptor, RewritesOnlyClassSegments)
{
    const auto keep = [](std::string_view name) -> std::string_view {
        if (name == "Tuple") return "l";
        return name;
    };

    EXPECT_EQ(dasho::rewriteDescriptor("()LTuple;", keep).unwrap(), "()Ll;");
    EXPECT_EQ(dasho::rewriteDescriptor("(LTuple;I)LTuple;", keep).unwrap(), "(Ll;I)Ll;");
    EXPECT_EQ(dasho::rewriteDescriptor("[[LTuple;", keep).unwrap(), "[[Ll;");
    // 命中不了的名字原样保留（JDK 类型不在 map 里）。
    EXPECT_EQ(dasho::rewriteDescriptor("(Ljava/lang/String;)V", keep).unwrap(),
              "(Ljava/lang/String;)V");
    // 纯原始类型没有类名段。
    EXPECT_EQ(dasho::rewriteDescriptor("(IJ)D", keep).unwrap(), "(IJ)D");
}

TEST(RewriteDescriptor, RejectsMalformed)
{
    const auto keep = [](std::string_view name) -> std::string_view { return name; };
    EXPECT_TRUE(dasho::rewriteDescriptor("(LTuple;", keep).is_err());
    EXPECT_TRUE(dasho::rewriteDescriptor("Q", keep).is_err());
}

}  // namespace
