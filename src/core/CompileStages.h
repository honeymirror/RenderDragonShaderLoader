#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace renderdragon_shader_loader {

inline std::string_view compileMacroName(std::string_view macro) {
    auto const equals = macro.find('=');
    return equals == std::string_view::npos ? macro : macro.substr(0, equals);
}

inline bool isColorStagingMacro(std::string_view macro) {
    static constexpr std::array<std::string_view, 0> kMacros = {};
    auto const name = compileMacroName(macro);
    for (auto const candidate : kMacros) {
        if (name == candidate) return true;
    }
    return false;
}

inline bool shouldStageColorCompile(std::vector<std::string> const& macros) {
    for (auto const& macro : macros) {
        if (isColorStagingMacro(macro)) return true;
    }
    return false;
}

inline bool isVisibleStageMaterial(std::string_view name) {
    static constexpr std::array<std::string_view, 14> kPrefixes = {
        "Bloom",
        "Clouds",
        "ColorPostProcessing",
        "DeferredMixedResolution",
        "DeferredShading",
        "DeferredWater",
        "EndSky",
        "Particle",
        "RenderChunk",
        "Sky",
        "Stars",
        "SunMoon",
        "Water",
        "Weather",
    };
    for (auto const prefix : kPrefixes) {
        if (name.starts_with(prefix)) return true;
    }
    return false;
}

inline void splitStagedMaterials(
    std::vector<std::string> const& all,
    std::vector<std::string>&       priority,
    std::vector<std::string>&       background
) {
    priority.clear();
    background.clear();
    for (auto const& material : all) {
        if (isVisibleStageMaterial(material)) {
            priority.push_back(material);
        } else {
            background.push_back(material);
        }
    }
}

} // namespace renderdragon_shader_loader
