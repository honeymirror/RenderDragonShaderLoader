#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ShaderParameter.h"

namespace renderdragon_shader_loader {

// rdloader.pack.json 里的一个作者预设。settings/bins 都是包内相对路径。
struct PackPresetManifest {
    std::string id;
    std::string label;
    std::string settings;
    std::string bins;
};

struct PackManifest {
    bool                     valid = false;
    int                      packVersion = 0;
    std::string              name;
    std::string              gameVersion;
    std::string              defaultPreset;
    std::string              backend;
    std::vector<std::string> profiles;
    std::vector<PackPresetManifest> presets;
};

// options.json 的字段会映射到 ShaderParameter。macroMap 保留给 select 类型。
struct PackOption {
    ParameterType              type = ParameterType::Bool;
    std::string                name;
    std::string                macro;
    std::map<std::string, double> macroMap;
    std::string                defaultValue;
    double                     min = 0.0;
    double                     max = 1.0;
    double                     step = 0.0;
    std::string                group;
    std::string                icon;
    std::string                kind = "source";
    std::string                langKey;
    std::string                material;
    std::string                uniform;
    std::string                uniformType = "vec4";
    std::vector<int>           components;
    std::vector<std::string>   values;
};

struct PackOptions {
    bool                     valid = false;
    std::vector<PackOption> options;
};

struct PackVariants {
    bool                     valid = false;
    std::vector<std::string> variantIds;
};

using PackLanguage = std::map<std::string, std::string>;

// 这些解析器只负责读取元数据；包文件缺失或格式错误不会抛异常，只会返回 valid=false。
PackManifest loadPackManifest(std::filesystem::path const& packRoot);
PackOptions  loadPackOptions(std::filesystem::path const& packRoot);
PackVariants loadPackVariants(std::filesystem::path const& packRoot);
PackLanguage loadPackLanguage(std::filesystem::path const& packRoot, std::string const& locale = "zh_CN");

// 把 options.json 转成现有 UI/编译器使用的 ShaderParameter 列表。
std::vector<ShaderParameter> optionsToShaderParameters(PackOptions const& options);

// 按 macro 查 options.json 里的定义；找不到返回 nullptr。
PackOption const* findPackOption(PackOptions const& options, std::string_view macro);

} // namespace renderdragon_shader_loader
