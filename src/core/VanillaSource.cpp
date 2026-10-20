#include "core/VanillaSource.h"

#include <system_error>
#include <vector>

#include "ll/api/mod/Mod.h"

namespace renderdragon_shader_loader {

std::filesystem::path detectGameMaterialsDir(std::filesystem::path const& modDir) {
    std::error_code ec;

    // 客户端里 getModDir() 可能是相对路径（实测为 ./mods/<模组名>），
    // 先取绝对路径再按 <版本目录>/mods/<模组名> 反推出 <版本目录>。
    auto absModDir = std::filesystem::absolute(modDir, ec);
    if (ec) absModDir = modDir;
    auto versionDir = absModDir.parent_path().parent_path();

    std::vector<std::filesystem::path> candidates{
        versionDir / "data" / "renderer" / "materials",
        ll::mod::getModsRoot() / ".." / "data" / "renderer" / "materials",
        absModDir / "vanilla",
    };

    for (auto const& candidate : candidates) {
        if (std::filesystem::is_directory(candidate, ec)) {
            auto resolved = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate : resolved;
        }
    }
    return {};
}

} // namespace renderdragon_shader_loader
