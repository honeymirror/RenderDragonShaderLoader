#include "core/PackMetadata.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace renderdragon_shader_loader {
namespace {

using nlohmann::json;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value;
}

ParameterType parseOptionType(std::string const& value) {
    auto const type = lower(value);
    if (type == "select" || type == "enum") return ParameterType::Enum;
    if (type == "slider") return ParameterType::Slider;
    if (type == "color") return ParameterType::Color;
    return ParameterType::Bool;
}

bool isUniformType(std::string const& value) {
    return value == "vec4" || value == "mat3" || value == "mat4" || value == "external";
}

ParameterType parseUniformControl(json const& value, std::vector<int> const& components) {
    if (value.contains("control") && value["control"].is_string()) {
        return parseOptionType(value["control"].get<std::string>());
    }
    if (value.contains("min") || value.contains("max")) return ParameterType::Slider;
    if (components.size() == 3) return ParameterType::Color;
    return ParameterType::Slider;
}

std::vector<int> parseComponents(json const& value) {
    std::vector<int> result;
    if (!value.contains("components") || !value["components"].is_array()) return result;
    for (auto const& item : value["components"]) {
        if (item.is_number_integer()) result.push_back(item.get<int>());
        else if (item.is_number_unsigned()) result.push_back((int)item.get<unsigned int>());
    }
    return result;
}

std::string valueToString(json const& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "ON" : "OFF";
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) {
        std::ostringstream out;
        out << value.get<double>();
        return out.str();
    }
    return {};
}

bool readJson(std::filesystem::path const& file, json& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    try {
        in >> out;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

PackManifest loadPackManifest(std::filesystem::path const& packRoot) {
    PackManifest result;
    json         root;
    if (!readJson(packRoot / "rdloader.pack.json", root) || !root.is_object()) return result;

    result.packVersion = root.value("packVersion", 0);
    result.name        = root.value("name", std::string{});
    result.gameVersion = root.value("gameVersion", std::string{});
    result.defaultPreset = root.value("defaultPreset", std::string{});
    result.backend = root.value("backend", std::string{});

    if (root.contains("profiles") && root["profiles"].is_array()) {
        for (auto const& item : root["profiles"]) {
            if (item.is_string()) result.profiles.push_back(item.get<std::string>());
        }
    }
    if (root.contains("presets") && root["presets"].is_array()) {
        for (auto const& item : root["presets"]) {
            if (!item.is_object()) continue;
            PackPresetManifest preset;
            preset.id       = item.value("id", std::string{});
            preset.label    = item.value("label", preset.id);
            preset.settings = item.value("settings", std::string{});
            preset.bins     = item.value("bins", std::string{});
            if (!preset.id.empty()) result.presets.push_back(std::move(preset));
        }
    }
    result.valid = result.packVersion > 0 && !result.presets.empty();
    return result;
}

PackOptions loadPackOptions(std::filesystem::path const& packRoot) {
    PackOptions result;
    json        root;
    if (!readJson(packRoot / "options.json", root)) return result;

    json const* list = nullptr;
    if (root.is_array()) {
        list = &root;
    } else if (root.is_object() && root.contains("options") && root["options"].is_array()) {
        list = &root["options"];
    } else if (root.is_object()) {
        // 兼容 { "MACRO": { ... } } 的紧凑写法。
        for (auto const& [key, value] : root.items()) {
            if (!value.is_object()) continue;
            PackOption option;
            option.name = key;
            option.macro = value.value("macro", key);
            option.kind = value.value("kind", std::string{"source"});
            option.material = value.value("material", std::string{});
            option.uniform = value.value("uniform", option.macro);
            option.components = parseComponents(value);
            auto declaredType = lower(value.value("type", std::string{"toggle"}));
            option.uniformType = lower(value.value(
                "uniformType",
                isUniformType(declaredType) ? declaredType : std::string{"vec4"}
            ));
            option.type = option.kind == "uniform" && isUniformType(declaredType)
                              ? parseUniformControl(value, option.components)
                              : parseOptionType(declaredType);
            if (value.contains("default")) option.defaultValue = valueToString(value["default"]);
            option.min = value.value("min", 0.0);
            option.max = value.value("max", 1.0);
            option.step = value.value("step", 0.0);
            option.group = value.value("group", std::string{});
            option.icon = value.value("icon", std::string{});
            option.langKey = value.value("langKey", std::string{});
            if (value.contains("macroMap") && value["macroMap"].is_object()) {
                for (auto const& [k, v] : value["macroMap"].items()) {
                    if (v.is_number()) option.macroMap[k] = v.get<double>();
                }
            }
            if (value.contains("values") && value["values"].is_array()) {
                for (auto const& item : value["values"]) {
                    if (item.is_string()) option.values.push_back(item.get<std::string>());
                }
            }
            result.options.push_back(std::move(option));
        }
        result.valid = true;
        return result;
    }

    if (list == nullptr) return result;
    for (auto const& value : *list) {
        if (!value.is_object()) continue;
        PackOption option;
        option.name = value.value("name", std::string{});
        option.macro = value.value("macro", option.name);
        option.kind = value.value("kind", std::string{"source"});
        option.material = value.value("material", std::string{});
        option.uniform = value.value("uniform", option.macro);
        option.components = parseComponents(value);
        auto declaredType = lower(value.value("type", std::string{"toggle"}));
        option.uniformType = lower(value.value(
            "uniformType",
            isUniformType(declaredType) ? declaredType : std::string{"vec4"}
        ));
        option.type = option.kind == "uniform" && isUniformType(declaredType)
                          ? parseUniformControl(value, option.components)
                          : parseOptionType(declaredType);
        if (value.contains("default")) option.defaultValue = valueToString(value["default"]);
        option.min = value.value("min", 0.0);
        option.max = value.value("max", 1.0);
        option.step = value.value("step", 0.0);
        option.group = value.value("group", std::string{});
        option.icon = value.value("icon", std::string{});
        option.langKey = value.value("langKey", std::string{});
        if (value.contains("macroMap") && value["macroMap"].is_object()) {
            for (auto const& [k, v] : value["macroMap"].items()) {
                if (v.is_number()) option.macroMap[k] = v.get<double>();
            }
        }
        if (value.contains("values") && value["values"].is_array()) {
            for (auto const& item : value["values"]) {
                if (item.is_string()) option.values.push_back(item.get<std::string>());
            }
        }
        if (!option.macro.empty()) result.options.push_back(std::move(option));
    }
    result.valid = !result.options.empty();
    return result;
}

PackVariants loadPackVariants(std::filesystem::path const& packRoot) {
    PackVariants result;
    json         root;
    if (!readJson(packRoot / "variants.json", root) || !root.is_object()) return result;

    if (root.contains("variants") && root["variants"].is_array()) {
        for (auto const& item : root["variants"]) {
            if (item.is_string()) result.variantIds.push_back(item.get<std::string>());
            else if (item.is_object() && item.contains("id") && item["id"].is_string()) {
                result.variantIds.push_back(item["id"].get<std::string>());
            }
        }
    }
    result.valid = !result.variantIds.empty();
    return result;
}

PackLanguage loadPackLanguage(std::filesystem::path const& packRoot, std::string const& locale) {
    PackLanguage result;
    std::error_code ec;

    // 语言代码要宽松匹配：游戏/系统给出的可能写成 zh-CN、zh_CN、en-US、en_US，
    // 包内文件则统一是 <locale>.lang。这里把写法差异都归一化。
    std::vector<std::string> candidates;
    if (!locale.empty()) {
        candidates.push_back(locale);
        std::string swapped = locale;
        std::replace(swapped.begin(), swapped.end(), '-', '_');
        candidates.push_back(std::move(swapped));
        std::string hyphenated = locale;
        std::replace(hyphenated.begin(), hyphenated.end(), '_', '-');
        candidates.push_back(std::move(hyphenated));
        // 只有语言、没有地区的（"en" -> en_US 之类）交给下面的回退处理。
    }
    candidates.push_back("en_US");
    candidates.push_back("zh_CN");

    std::filesystem::path file;
    for (auto const& candidate : candidates) {
        auto probe = packRoot / "lang" / (candidate + ".lang");
        if (std::filesystem::is_regular_file(probe, ec)) {
            file = probe;
            break;
        }
    }
    if (file.empty()) return result;

    std::ifstream in(file, std::ios::binary);
    if (!in) return result;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#' || line[first] == ';') continue;
        auto eq = line.find('=', first);
        if (eq == std::string::npos) continue;
        auto key = line.substr(first, eq - first);
        auto value = line.substr(eq + 1);
        auto trim = [](std::string value) {
            auto a = value.find_first_not_of(" \t");
            if (a == std::string::npos) return std::string{};
            auto b = value.find_last_not_of(" \t");
            return value.substr(a, b - a + 1);
        };
        result[trim(key)] = trim(value);
    }
    return result;
}

std::vector<ShaderParameter> optionsToShaderParameters(PackOptions const& options) {
    std::vector<ShaderParameter> result;
    for (auto const& option : options.options) {
        ShaderParameter parameter;
        // options.json 可以只写 macro（适配包就是这么生成的）。这时 name 必须回退到
        // macro —— 否则参数名是空串，保存出来的设置文件全是 "=0.2" 这种没有键名的行，
        // 读回时匹配不上任何参数，玩家调的值每次都被丢掉（表现：怎么调都没效果）。
        parameter.macro = option.macro.empty() ? option.name : option.macro;
        parameter.name  = option.name.empty() ? parameter.macro : option.name;
        if (parameter.name.empty()) continue;
        parameter.label = option.langKey.empty() ? parameter.name : option.langKey;
        parameter.langKey = option.langKey;
        parameter.group = option.group;
        parameter.icon = option.icon;
        parameter.kind = option.kind;
        parameter.macroMap = option.macroMap;
        parameter.type = option.type;
        parameter.material = option.material;
        parameter.uniform = option.uniform;
        parameter.uniformType = option.uniformType;
        parameter.components = option.components;
        parameter.min = option.min;
        parameter.max = option.max;
        parameter.current = option.defaultValue;
        if (option.type == ParameterType::Slider) {
            parameter.def = option.min;
            try {
                if (!option.defaultValue.empty()) parameter.def = std::stod(option.defaultValue);
            } catch (...) {
            }
            if (parameter.current.empty()) parameter.current = std::to_string(parameter.def);
            if (option.step > 0.0) parameter.integer = option.step >= 0.999;
        } else if (option.type == ParameterType::Enum) {
            if (!option.values.empty()) parameter.values = option.values;
            else {
                for (auto const& [key, value] : option.macroMap) {
                    (void)value;
                    parameter.values.push_back(key);
                }
            }
            if (parameter.current.empty() && !parameter.values.empty()) parameter.current = parameter.values.front();
        }
        if (parameter.kind == "uniform" && parameter.current.empty()
            && parameter.type == ParameterType::Slider) {
            parameter.current = std::to_string(parameter.min);
        }
        parameter.defaultValue = parameter.current;
        result.push_back(std::move(parameter));
    }
    return result;
}

PackOption const* findPackOption(PackOptions const& options, std::string_view macro) {
    for (auto const& option : options.options) {
        if (option.macro == macro || option.name == macro || option.uniform == macro) return &option;
    }
    return nullptr;
}

} // namespace renderdragon_shader_loader
