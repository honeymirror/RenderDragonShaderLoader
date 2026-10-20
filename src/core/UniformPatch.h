#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "core/ShaderParameter.h"

namespace renderdragon_shader_loader {

// A component-level patch for an existing inline vec4 uniform.
struct UniformPatch {
    std::string      material;
    std::string      uniform;
    std::string      type = "vec4";
    std::vector<int> components;
    std::vector<float> values;
};

inline double parameterNumber(ShaderParameter const& parameter, bool* ok = nullptr) {
    if (ok) *ok = true;
    if (parameter.type == ParameterType::Bool) {
        auto value = parameter.current;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });
        return value == "on" || value == "true" || value == "yes" || value == "1" ? 1.0 : 0.0;
    }
    if (parameter.type == ParameterType::Enum) {
        if (auto it = parameter.macroMap.find(parameter.current); it != parameter.macroMap.end()) {
            return it->second;
        }
        auto it = std::find(parameter.values.begin(), parameter.values.end(), parameter.current);
        if (it != parameter.values.end()) return (double)std::distance(parameter.values.begin(), it);
        if (ok) *ok = false;
        return 0.0;
    }
    char* end = nullptr;
    double value = std::strtod(parameter.current.c_str(), &end);
    if (end == parameter.current.c_str() || *end != '\0' || !std::isfinite(value)) {
        if (ok) *ok = false;
        return 0.0;
    }
    return value;
}

inline bool parseHexColor(std::string const& text, double& r, double& g, double& b) {
    if (text.size() != 7 || text.front() != '#') return false;
    char* end = nullptr;
    unsigned long value = std::strtoul(text.c_str() + 1, &end, 16);
    if (end == text.c_str() + 1 || *end != '\0') return false;
    r = (double)((value >> 16) & 0xFF) / 255.0;
    g = (double)((value >> 8) & 0xFF) / 255.0;
    b = (double)(value & 0xFF) / 255.0;
    return true;
}

// Convert one UI parameter to the runtime patch contract. Invalid declarations fail
// closed so malformed pack metadata cannot silently write the wrong component.
inline std::optional<UniformPatch> makeUniformPatch(
    ShaderParameter const& parameter,
    std::string*           error = nullptr
) {
    auto fail = [&](std::string message) -> std::optional<UniformPatch> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };

    if (parameter.kind != "uniform") return std::nullopt;
    if (parameter.material.empty()) return fail("uniform parameter has no material");
    if (parameter.uniform.empty()) return fail("uniform parameter has no uniform name");
    if (parameter.uniformType != "vec4") {
        return fail("only vec4 uniforms are supported");
    }
    if (parameter.components.empty()) return fail("uniform parameter has no components");

    std::vector<int> components = parameter.components;
    std::sort(components.begin(), components.end());
    if (std::adjacent_find(components.begin(), components.end()) != components.end()) {
        return fail("uniform parameter has duplicate components");
    }
    for (int component : components) {
        if (component < 0 || component > 3) {
            return fail("uniform component must be in 0..3");
        }
    }

    UniformPatch patch;
    patch.material = parameter.material;
    patch.uniform  = parameter.uniform;
    patch.type     = parameter.uniformType;
    patch.components = parameter.components;

    if (parameter.type == ParameterType::Color) {
        if (patch.components.size() != 3) {
            return fail("color uniform parameters require exactly 3 components");
        }
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        if (!parseHexColor(parameter.current, r, g, b)) {
            return fail("color value must use #RRGGBB");
        }
        patch.values = {(float)r, (float)g, (float)b};
        return patch;
    }

    bool ok = false;
    double value = parameterNumber(parameter, &ok);
    if (!ok) return fail("uniform parameter value is not numeric");
    patch.values.assign(patch.components.size(), (float)value);
    return patch;
}

inline bool collectUniformPatches(
    std::vector<ShaderParameter> const& parameters,
    std::vector<UniformPatch>&          patches,
    std::string&                        error
) {
    patches.clear();
    for (auto const& parameter : parameters) {
        if (parameter.kind != "uniform") continue;
        std::string patchError;
        auto patch = makeUniformPatch(parameter, &patchError);
        if (!patch) {
            error = "uniform parameter '" + parameter.name + "': " + patchError;
            return false;
        }
        patches.push_back(std::move(*patch));
    }
    return true;
}

} // namespace renderdragon_shader_loader
