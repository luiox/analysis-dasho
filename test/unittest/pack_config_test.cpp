/// pack_config 单测：配置面只认扁平字符串键，但两种手写形态都得能跑。
///
/// 重点是 Windows 路径：`"D:\lab\x.map"` 里的 `\x` 不是合法 JSON 转义，严格
/// 解析器会拒。这里刻意按字面收——用户不会为了写个路径去查 JSON 规范。
#include <gtest/gtest.h>

#include <string>

#include "dasho/pack_config.hpp"

namespace {

/// 反斜杠在源码里一律写成转义形式，避免被工具链的路径转换吃掉。
const std::string kBs = "\\";   // 一个反斜杠

TEST(PackConfig, ReadsPlainKey)
{
    std::string out;
    EXPECT_TRUE(dasho::configStringField(R"({"map": "a.map"})", "map", out));
    EXPECT_EQ(out, "a.map");
}

TEST(PackConfig, ToleratesSpacesAndOrder)
{
    std::string out;
    EXPECT_TRUE(dasho::configStringField(R"(  { "x" : 1 , "map" : "b.map" } )", "map", out));
    EXPECT_EQ(out, "b.map");
}

TEST(PackConfig, KeepsUnescapedWindowsPath)
{
    // 输入形如 {"map": "D:\lab\a.map"}：\W 不是合法转义，按字面收。
    const std::string json = "{\"map\": \"D:" + kBs + "lab" + kBs + "a.map\"}";
    std::string       out;
    EXPECT_TRUE(dasho::configStringField(json, "map", out));
    EXPECT_EQ(out, "D:" + kBs + "lab" + kBs + "a.map");
}

TEST(PackConfig, DecodesEscapedWindowsPath)
{
    // 输入形如 {"map": "D:\\lab\\a.map"}：合法转义，解回单个反斜杠。
    const std::string json =
        "{\"map\": \"D:" + kBs + kBs + "lab" + kBs + kBs + "a.map\"}";
    std::string out;
    EXPECT_TRUE(dasho::configStringField(json, "map", out));
    EXPECT_EQ(out, "D:" + kBs + "lab" + kBs + "a.map");
}

TEST(PackConfig, MissingKeyReturnsFalse)
{
    std::string out = "untouched";
    EXPECT_FALSE(dasho::configStringField(R"({"other": "x"})", "map", out));
    EXPECT_EQ(out, "untouched");
}

TEST(PackConfig, NonStringValueSkipped)
{
    std::string out;
    // 同名键先出现数值形态，后出现字符串形态：取后者。
    EXPECT_TRUE(dasho::configStringField(R"({"map": 1, "map": "c.map"})", "map", out));
    EXPECT_EQ(out, "c.map");
}

TEST(PackConfig, EmptyConfig)
{
    std::string out;
    EXPECT_FALSE(dasho::configStringField("", "map", out));
}

}  // namespace
