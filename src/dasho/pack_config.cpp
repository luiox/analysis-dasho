#include "dasho/pack_config.hpp"

#include <utility>

namespace dasho {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// 从 `pos`（指向开引号）读一个 JSON 字符串字面量，解码到 out。
/// 返回闭引号之后的位置；未闭合返回 npos。
std::size_t readString(std::string_view text, std::size_t pos, std::string& out)
{
    if (pos >= text.size() || text[pos] != '"') return std::string_view::npos;
    ++pos;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '"') return pos + 1;
        if (c != '\\') {
            out.push_back(c);
            ++pos;
            continue;
        }
        if (pos + 1 >= text.size()) return std::string_view::npos;
        const char esc = text[pos + 1];
        switch (esc) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            default:
                // 未转义的反斜杠（典型：Windows 路径）：两个字符都按字面收。
                out.push_back('\\');
                out.push_back(esc);
                break;
        }
        pos += 2;
    }
    return std::string_view::npos;
}

}  // namespace

bool configStringField(std::string_view text, std::string_view key, std::string& out)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t       from   = 0;
    while (true) {
        const std::size_t hit = text.find(needle, from);
        if (hit == std::string_view::npos) return false;
        from = hit + needle.size();

        // 冒号
        std::size_t pos = from;
        while (pos < text.size() && isSpace(text[pos])) ++pos;
        if (pos >= text.size() || text[pos] != ':') continue;
        ++pos;
        while (pos < text.size() && isSpace(text[pos])) ++pos;

        // 值必须是字符串字面量；不是就继续找下一个同名键。
        std::string value;
        if (readString(text, pos, value) == std::string_view::npos) continue;
        out = std::move(value);
        return true;
    }
}

}  // namespace dasho
