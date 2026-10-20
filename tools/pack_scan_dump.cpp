// 离线检查 ShaderPackStore::scan 的结果（不启动游戏）：
//   pack_scan_dump.exe <shaderpacks 目录> <generated 目录> [presets 目录]
// 打印每个包解析出的 sourceDir / binsDir / prebuiltDir / renderMode / 参数个数，
// 用来排查"包内自带的 .material.bin 有没有被收进 compiled/"。

#include <cstdio>
#include <filesystem>
#include <string>

#include "core/ShaderPack.h"

using namespace renderdragon_shader_loader;

namespace {

std::size_t countBins(std::filesystem::path const& dir) {
    std::error_code ec;
    std::size_t     n = 0;
    for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().filename().string().ends_with(".material.bin")) ++n;
    }
    return n;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: pack_scan_dump <shaderpacksDir> <generatedDir> [presetsDir]\n");
        return 2;
    }
    ShaderPackStore::getInstance().scan(argv[1], argv[2], argc > 3 ? argv[3] : std::filesystem::path{});

    for (auto const& p : ShaderPackStore::getInstance().packs()) {
        std::printf(
            "pack '%s'\n  sourceDir   = %s\n  binsDir     = %s  (%zu bin)\n  prebuiltDir = %s\n"
            "  renderMode  = %s\n  parameters  = %zu\n",
            p.name.c_str(),
            p.sourceDir.string().c_str(),
            p.binsDir.string().c_str(),
            countBins(p.binsDir),
            p.prebuiltDir.string().c_str(),
            p.renderMode == ShaderRenderMode::Deferred ? "Deferred" : "Forward",
            p.parameters.size()
        );
        std::printf("  currentPreset = %s\n", p.currentPreset.c_str());
        for (auto const& preset : p.presets) {
            std::printf(
                "  preset '%s' label='%s' origin=%s compiled=%d compatible=%d bins=%s\n",
                preset.id.c_str(),
                preset.label.c_str(),
                preset.origin.c_str(),
                (int)preset.compiled,
                (int)preset.compatible,
                preset.binsDir.string().c_str()
            );
        }
        // 顺手把"包里各个候选产物目录"都列出来，确认扫描到底看到了什么。
        auto listSub = [&](char const* label, std::filesystem::path const& d) {
            std::error_code ec;
            std::printf(
                "  [%s] %s : isDir=%d bins=%zu\n",
                label,
                d.string().c_str(),
                (int)std::filesystem::is_directory(d, ec),
                countBins(d)
            );
        };
        listSub("base", p.sourceDir / "renderer" / "materials");
        std::error_code subEc;
        for (auto const& sub : std::filesystem::directory_iterator(p.sourceDir / "subpacks", subEc)) {
            if (sub.is_directory(subEc)) listSub("subpack", sub.path() / "renderer" / "materials");
        }
    }
    if (ShaderPackStore::getInstance().packs().empty()) std::printf("(no packs)\n");
    return 0;
}
