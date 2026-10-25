#include "dasho/pack.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#include "dasho/checks_pass.hpp"
#include "dasho/cleanup_pass.hpp"
#include "dasho/control_flow_pass.hpp"
#include "dasho/pack_config.hpp"
#include "dasho/rename_map.hpp"
#include "dasho/rename_pass.hpp"
#include "dasho/string_decrypt_pass.hpp"

namespace dasho {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

/// 读整份文本文件；失败返回空串。
std::string readTextFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// 家族 pack：manifest 声明构建戳与命名空间，注册委托各变换件的
/// registerXxx。
///
/// 配置面（`-c` 交给 pack 的文本，扁平 JSON）：
///   - `"map": "<path>"` —— 改名映射文件（DashO `.map` 或 ProGuard `.map`）。
///     给了才注册 `dasho.Rename`：改名不可逆推，没有 mapfile 就没有还原依据，
///     注册一个必然空转的 pass 只会误导 `--list`。
class DashoPack final : public mjbase::plugin::IPassPack
{
public:
    const mjbase::plugin::PackManifest& manifest() const override { return manifest_; }

    Result<void, mjbase::plugin::PackError> registerPasses(mjbase::spi::PassNameRegistry& registry,
                                                           std::string_view config) override
    {
        if (Result<void, std::string> r = registerStringDecrypt(registry); r.is_err()) {
            return Err(mjbase::plugin::makePackError(
                mjbase::plugin::PackErrorKind::RegistrationFailed, r.unwrap_err()));
        }
        if (Result<void, std::string> r = registerControlFlow(registry); r.is_err()) {
            return Err(mjbase::plugin::makePackError(
                mjbase::plugin::PackErrorKind::RegistrationFailed, r.unwrap_err()));
        }
        if (Result<void, std::string> r = registerChecks(registry); r.is_err()) {
            return Err(mjbase::plugin::makePackError(
                mjbase::plugin::PackErrorKind::RegistrationFailed, r.unwrap_err()));
        }
        if (Result<void, std::string> r = registerCleanup(registry); r.is_err()) {
            return Err(mjbase::plugin::makePackError(
                mjbase::plugin::PackErrorKind::RegistrationFailed, r.unwrap_err()));
        }

        std::string mapPath;
        if (configStringField(config, "map", mapPath)) {
            if (Result<void, std::string> r = registerRenameFromFile(registry, mapPath); r.is_err()) {
                return Err(mjbase::plugin::makePackError(
                    mjbase::plugin::PackErrorKind::RegistrationFailed, r.unwrap_err()));
            }
        }
        return Ok();
    }

private:
    static Result<void, std::string> registerRenameFromFile(mjbase::spi::PassNameRegistry& registry,
                                                            const std::string&             path)
    {
        const std::string text = readTextFile(path);
        if (text.empty()) return Err("mapfile 为空或不可读：" + path);

        auto parsed = parseRenameMap(text);
        if (parsed.is_err()) return Err("mapfile 解析失败：" + parsed.unwrap_err());

        return registerRename(registry, std::make_shared<const RenameMap>(std::move(parsed.unwrap())));
    }

    mjbase::plugin::PackManifest manifest_{
        "dasho", mjbase::plugin::buildCommit(),
        "DashO obfuscation family deobfuscation (string encryption, control flow, checks, cleanup, renaming)."};
};

}  // namespace

mjbase::plugin::IPassPack& dasho_pack()
{
    static DashoPack pack;
    return pack;
}

}  // namespace dasho
