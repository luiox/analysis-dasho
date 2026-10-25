/// @file        main.cpp
/// @brief       dasho CLI：装 pack → 导 jar → 管线 → 导 jar。
///
/// 各插件仓自组 CLI：mjbase 出框架（注册表/管线/容器 IO/workspace 装配），
/// 本仓只出 pack 与 pass。装配走 plugin-api.md 宿主配方的静态链接形态。
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "dasho/pack.hpp"
#include "dasho/rename_map.hpp"
#include "mjbase/log/log_init.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/plugin/pack.hpp"
#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"

namespace {

constexpr const char* kUsage =
    "dasho_cli -i in.jar -o out.jar -p dasho.X[,dasho.Y...] [-c pack.json] [-m map.txt]\n"
    "  -i  输入 jar\n"
    "  -o  输出 jar\n"
    "  -p  逗号分隔 pass 名（dasho. 前缀）\n"
    "  -c  pack 配置 JSON 文件（可选，文本整体交给 pack 解析）\n"
    "  -m  改名映射文件（DashO .map / ProGuard .map）；等价于 -c 里写 {\"map\":\"<路径>\"}\n"
    "      —— 只有给了 mapfile，dasho.Rename 才会注册\n"
    "  --map-check <file>  解析改名映射文件并报统计后退出（不跑管线）\n"
    "  --list  列出本 pack 注册的全部 pass 后退出\n";

std::string readFileText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// 路径 → JSON 字符串字面量内容（转义 `\` 与 `"`）。
std::string jsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> splitNames(const std::string& list)
{
    std::vector<std::string> out;
    std::string item;
    std::istringstream ss(list);
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

}   // namespace

int main(int argc, char** argv)
{
    mjbase::log::initDefaultLogger();

    std::string input;
    std::string output;
    std::string passList;
    std::string configPath;
    std::string mapCheckPath;
    std::string mapPath;
    bool        list = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            list = true;
        } else if (arg == "--map-check" && i + 1 < argc) {
            mapCheckPath = argv[++i];
        } else if ((arg == "-m" || arg == "--map") && i + 1 < argc) {
            mapPath = argv[++i];
        } else if (arg == "-i" && i + 1 < argc) {
            input = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            passList = argv[++i];
        } else if (arg == "-c" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "未知参数：%s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }

    std::string configText;
    if (!configPath.empty()) {
        configText = readFileText(configPath);
        if (configText.empty()) {
            std::fprintf(stderr, "配置文件为空或不可读：%s\n", configPath.c_str());
            return 2;
        }
    }
    if (!mapPath.empty()) {
        if (!configPath.empty()) {
            std::fputs("-m 与 -c 互斥：-m 等价于 -c 里的 map 项，二选一\n", stderr);
            return 2;
        }
        configText = "{\"map\": \"" + jsonEscape(mapPath) + "\"}";
    }

    // 装配（plugin-api.md 宿主配方）：宿主自持注册表，逐 pack 装入，
    // dasho. 前缀与同源门由 installPack 机械验收。
    mjbase::spi::PassNameRegistry registry;
    if (ca::core::Result<void, mjbase::plugin::PackError> r =
            mjbase::plugin::installPack(dasho::dasho_pack(), registry, configText);
        r.is_err()) {
        std::fprintf(stderr, "pack 装配失败：%s：%s\n", mjbase::plugin::to_string(r.unwrap_err().kind),
                     r.unwrap_err().detail.c_str());
        return 2;
    }

    if (!mapCheckPath.empty()) {
        const std::string text = readFileText(mapCheckPath);
        if (text.empty()) {
            std::fprintf(stderr, "映射文件为空或不可读：%s\n", mapCheckPath.c_str());
            return 2;
        }
        auto parsed = dasho::parseRenameMap(text);
        if (parsed.is_err()) {
            std::fprintf(stderr, "解析失败：%s\n", parsed.unwrap_err().c_str());
            return 2;
        }
        const dasho::RenameMap& map = parsed.unwrap();
        ca::usize               moved = 0;
        for (const auto& [oldName, newName] : map.classes) {
            if (oldName != newName) ++moved;
        }
        std::printf("类 %zu 条（改名 %zu，保留 %zu）\n", map.classes.size(), moved,
                    map.unchangedClasses);
        std::printf("成员 %zu 条（改名 %zu，保留 %zu）\n", map.members.size(),
                    map.members.size() - map.unchangedMembers, map.unchangedMembers);
        if (map.skippedLines != 0) std::printf("跳过 %zu 行（语法不合）\n", map.skippedLines);
        return 0;
    }

    if (list) {
        for (const auto& entry : registry.list()) {
            if (entry.name.substr(0, 6) == "dasho.") {
                std::printf("%.*s\t%.*s\n", static_cast<int>(entry.name.size()), entry.name.data(),
                            static_cast<int>(entry.description.size()), entry.description.data());
            }
        }
        return 0;
    }

    if (input.empty() || output.empty() || passList.empty()) {
        std::fputs(kUsage, stderr);
        return 2;
    }

    mjbase::spi::PipelineBuilder builder;
    for (const std::string& name : splitNames(passList)) {
        if (auto r = mjbase::spi::applyTo(registry, name, builder); r.is_err()) {
            std::fprintf(stderr, "pass 装配失败：%s（%s）\n", name.c_str(),
                         mjbase::spi::to_string(r.unwrap_err()));
            return 2;
        }
    }

    mjbase::model::ResourceContainer container;
    if (auto m = mjbase::model::ResourceHelper::importFromJar(container, input); m.is_err()) {
        std::fprintf(stderr, "jar 导入失败（%s）：%s\n", mjbase::model::to_string(m.unwrap_err()),
                     input.c_str());
        return 2;
    }

    mjbase::spi::PassJobContext job;
    auto gpm = builder.buildNestedPipeline(job);
    if (auto r = mjbase::spi::runContainerPipeline(container, gpm, job); r.is_err()) {
        std::fprintf(stderr, "管线失败（%s@%s）：%s\n", mjbase::spi::to_string(r.unwrap_err().stage),
                     r.unwrap_err().target.c_str(), r.unwrap_err().detail.c_str());
        return 3;
    }

    if (auto m = mjbase::model::ResourceHelper::exportToJar(container, output); m.is_err()) {
        std::fprintf(stderr, "jar 导出失败（%s）：%s\n", mjbase::model::to_string(m.unwrap_err()),
                     output.c_str());
        return 2;
    }
    return 0;
}
