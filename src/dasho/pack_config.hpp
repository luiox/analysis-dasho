/// @file        pack_config.hpp
/// @brief       pack 配置文本的**字符串字段**读取（不实现完整 JSON）。
///
/// 本 pack 的配置面只有少量扁平字符串键（当前只有 `map`），为此拉一个 JSON
/// 依赖不划算。这里只做"找到 `"key"` 再取紧随的字符串字面量"这一件事，够用
/// 且可测；需要嵌套/数组/数值时再换真解析器。
///
/// 两处刻意的宽容（都是为了让手写配置不出事）：
///   - **未转义的反斜杠按字面收**。Windows 路径 `"D:\lab\x.map"` 里的 `\x`
///     不是合法 JSON 转义，严格解析器会拒；这里保留 `\x` 原样，配置能直接跑。
///   - **未知转义不报错**，原样保留（同上）。
#pragma once

#include <string>
#include <string_view>

namespace dasho {

/// 取顶层任意位置的 `"key": "value"` 字符串值。
///
/// @param out 命中时写入解码后的值；未命中不改写。
/// @return 是否命中。
bool configStringField(std::string_view text, std::string_view key, std::string& out);

}  // namespace dasho
