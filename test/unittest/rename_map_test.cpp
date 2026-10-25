/// RenameMap 单测：两种 mapfile 格式必须读成**同一张计划表**。
///
/// 这是改名还原链的第一环，判据只有一条——DashO 私有 `.map` 与 ProGuard 兼容
/// `.proguard.map` 由同一次构建同时产出，两份文件描述的是同一组映射。所以
/// 「两种格式解析结果逐条相等」是最强的自校验：任何一侧的描述符转换写错
/// （Java 风格 `int[]` → `[I`、`void` → `V`、点分名 → 斜杠名），交叉比对立刻炸。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "dasho/rename_map.hpp"

namespace {

/// 与 run-lab 里 `SnakeGame-renaming-default` 同构的片段（DashO 私有格式）。
constexpr const char* kDashoText = R"(|Window|f|
=<init>=()V=<init>=
=<clinit>=()V=<clinit>=
+serialVersionUID+J+serialVersionUID+
+Grid+Ljava/util/ArrayList;+a+
+width+I+b+

|DataOfSquare|a|
=<init>=(I)V=<init>=
=lightMeUp=(I)V=a=
+C+Ljava/util/ArrayList;+a+

|Main|Main|
=<init>=()V=<init>=
=main=([Ljava/lang/String;)V=a=
)";

/// 同一组映射的 ProGuard 兼容写法。
constexpr const char* kProguardText = R"(Window -> f:
    long serialVersionUID -> serialVersionUID
    java.util.ArrayList Grid -> a
    int width -> b
    void <init>() -> <init>
    void <clinit>() -> <clinit>
DataOfSquare -> a:
    java.util.ArrayList C -> a
    void <init>(int) -> <init>
    void lightMeUp(int) -> a
Main -> Main:
    void <init>() -> <init>
    void main(java.lang.String[]) -> a
)";

/// 把计划表拍成可比较的规范文本（排序后逐条比对）。
std::vector<std::string> canonicalMembers(const dasho::RenameMap& map)
{
    std::vector<std::string> out;
    out.reserve(map.members.size());
    for (const dasho::MemberRename& m : map.members) {
        out.push_back(m.owner + "|" + m.oldName + m.desc + "->" + m.newName
                      + (m.isMethod ? "|M" : "|F"));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}   // namespace

TEST(RenameMapTest, ParsesDashoPrivateFormat)
{
    auto parsed = dasho::parseDashoMap(kDashoText);
    ASSERT_TRUE(parsed.is_ok());
    const dasho::RenameMap& map = parsed.unwrap();

    EXPECT_EQ(map.classes.size(), 3u);
    EXPECT_EQ(map.classNewName("Window"), "f");
    EXPECT_EQ(map.classNewName("DataOfSquare"), "a");
    EXPECT_EQ(map.classNewName("Main"), "Main");   // 入口点保留原名
    EXPECT_TRUE(map.classNewName("Nope").empty());

    EXPECT_EQ(map.memberNewName("DataOfSquare", "lightMeUp", "(I)V"), "a");
    EXPECT_EQ(map.memberNewName("DataOfSquare", "C", "Ljava/util/ArrayList;"), "a");
    EXPECT_EQ(map.memberNewName("Main", "main", "([Ljava/lang/String;)V"), "a");
    // 未在表里的成员 → 空（调用方据此保持原名）。
    EXPECT_TRUE(map.memberNewName("Window", "notThere", "I").empty());
    // 描述符参与键：同名不同签名必须区分开。
    EXPECT_TRUE(map.memberNewName("DataOfSquare", "lightMeUp", "()V").empty());
}

TEST(RenameMapTest, ParsesProguardFormatToSamePlan)
{
    auto dashoParsed = dasho::parseDashoMap(kDashoText);
    auto pgParsed    = dasho::parseProguardMap(kProguardText);
    ASSERT_TRUE(dashoParsed.is_ok());
    ASSERT_TRUE(pgParsed.is_ok());

    const dasho::RenameMap& a = dashoParsed.unwrap();
    const dasho::RenameMap& b = pgParsed.unwrap();

    // 类表逐条相等。
    EXPECT_EQ(a.classes, b.classes);
    EXPECT_EQ(a.unchangedClasses, b.unchangedClasses);
    EXPECT_EQ(a.unchangedMembers, b.unchangedMembers);

    // 成员表逐条相等（含描述符——Java 风格签名转描述符的正确性全押在这条上）。
    const std::vector<std::string> ca = canonicalMembers(a);
    const std::vector<std::string> cb = canonicalMembers(b);
    ASSERT_EQ(ca.size(), cb.size());
    for (ca::usize i = 0; i < ca.size(); ++i) {
        EXPECT_EQ(ca[i], cb[i]) << "第 " << i << " 条不一致";
    }
}

TEST(RenameMapTest, ConvertsJavaTypesToDescriptors)
{
    EXPECT_EQ(dasho::javaTypeToDesc("void"), "V");
    EXPECT_EQ(dasho::javaTypeToDesc("int"), "I");
    EXPECT_EQ(dasho::javaTypeToDesc("long"), "J");
    EXPECT_EQ(dasho::javaTypeToDesc("boolean"), "Z");
    EXPECT_EQ(dasho::javaTypeToDesc("byte"), "B");
    EXPECT_EQ(dasho::javaTypeToDesc("char"), "C");
    EXPECT_EQ(dasho::javaTypeToDesc("short"), "S");
    EXPECT_EQ(dasho::javaTypeToDesc("float"), "F");
    EXPECT_EQ(dasho::javaTypeToDesc("double"), "D");
    EXPECT_EQ(dasho::javaTypeToDesc("java.lang.String"), "Ljava/lang/String;");
    EXPECT_EQ(dasho::javaTypeToDesc("java.lang.String[]"), "[Ljava/lang/String;");
    EXPECT_EQ(dasho::javaTypeToDesc("int[]"), "[I");
    EXPECT_EQ(dasho::javaTypeToDesc("int[][]"), "[[I");
    // 已是描述符的形态原样透传（两格式混用时不会被二次包裹）。
    EXPECT_EQ(dasho::javaTypeToDesc("Ljava/util/List;"), "Ljava/util/List;");
    EXPECT_EQ(dasho::javaTypeToDesc("[I"), "[I");
    // 点分 → 斜杠是类名归一，不是包扁平化。
    EXPECT_EQ(dasho::javaTypeToDesc("com.foo.Bar"), "Lcom/foo/Bar;");
}

TEST(RenameMapTest, CountsUnchangedEntries)
{
    auto parsed = dasho::parseDashoMap(kDashoText);
    ASSERT_TRUE(parsed.is_ok());
    const dasho::RenameMap& map = parsed.unwrap();

    EXPECT_EQ(map.unchangedClasses, 1u);   // Main -> Main
    // <init> ×3 + <clinit> ×1 + serialVersionUID ×1
    EXPECT_EQ(map.unchangedMembers, 5u);
    EXPECT_EQ(map.members.size(), 10u);
}

TEST(RenameMapTest, SkipsMalformedLinesWithoutFailing)
{
    constexpr const char* text = R"(|Window|f|
=garbage
+nope+nope
=keyPressed=(Ljava/awt/event/KeyEvent;)V=keyPressed=
|Broken|
)";
    auto parsed = dasho::parseDashoMap(text);
    ASSERT_TRUE(parsed.is_ok());
    const dasho::RenameMap& map = parsed.unwrap();

    EXPECT_EQ(map.classes.size(), 1u);
    EXPECT_EQ(map.members.size(), 1u);
    EXPECT_EQ(map.memberNewName("Window", "keyPressed", "(Ljava/awt/event/KeyEvent;)V"),
              "keyPressed");
    EXPECT_GT(map.skippedLines, 0u);
}

TEST(RenameMapTest, RejectsUnknownFormat)
{
    auto parsed = dasho::parseRenameMap("this is not a map file\nat all\n");
    EXPECT_TRUE(parsed.is_err());
}
