#include "core/MacroDependency.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "core/Hashing.h"

namespace renderdragon_shader_loader {
namespace {

using nlohmann::json;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value;
}

bool isIdentChar(char c) { return std::isalnum((unsigned char)c) != 0 || c == '_'; }

std::string trim(std::string value) {
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool isShaderSource(std::filesystem::path const& file) {
    auto ext = lower(file.extension().string());
    // 部分原生 GLSL 项目使用 shaders/shader.{frag,vert,comp}（不再是 .sc），
    // 两种都要认，否则依赖图会把整包标成"扫不到源码"→ 保守重编所有材质。
    return ext == ".sc" || ext == ".glsl" || ext == ".h" || ext == ".inc" || ext == ".frag"
        || ext == ".vert" || ext == ".comp";
}

std::vector<std::string> identifiersIn(std::string const& expression) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < expression.size();) {
        if (!isIdentChar(expression[i]) || std::isdigit((unsigned char)expression[i])) {
            ++i;
            continue;
        }
        auto start = i;
        while (i < expression.size() && isIdentChar(expression[i])) ++i;
        result.push_back(expression.substr(start, i - start));
    }
    return result;
}

std::vector<std::filesystem::path> readIncludePaths(std::filesystem::path const& packDir) {
    std::vector<std::filesystem::path> result{packDir / "include"};
    std::ifstream                       in(packDir / "project.json");
    if (!in) return result;
    try {
        json root;
        in >> root;
        if (root.contains("base_profile") && root["base_profile"].is_object()) {
            auto const& base = root["base_profile"];
            if (base.contains("include_search_paths") && base["include_search_paths"].is_array()) {
                for (auto const& item : base["include_search_paths"]) {
                    if (item.is_string()) result.push_back(packDir / item.get<std::string>());
                }
            }
        }
    } catch (...) {
    }
    return result;
}

void scanFile(
    std::filesystem::path const&                    file,
    std::vector<std::filesystem::path> const&        includePaths,
    std::set<std::string>&                          macros,
    std::set<std::filesystem::path>&                visited
) {
    std::error_code ec;
    auto            canonical = std::filesystem::weakly_canonical(file, ec);
    if (ec) canonical = file;
    if (!visited.insert(canonical).second) return;

    std::ifstream in(file, std::ios::binary);
    if (!in) return;

    // 本文件自己 `#define` 出来的宏不算依赖。
    // config.h / config.user.h 是"参数默认值表"，把 80 个参数全 define 了一遍；
    // 如果把这些定义也算成依赖，每个材质都会"依赖全部参数"，改一个滑条就要整包重编。
    std::set<std::string> definedHere;
    {
        std::string line;
        while (std::getline(in, line)) {
            auto trimmed = trim(line);
            if (trimmed.size() < 2 || trimmed[0] != '#') continue;
            auto rest = trim(trimmed.substr(1));
            if (rest.rfind("define", 0) != 0) continue;
            auto space = rest.find_first_of(" \t");
            if (space == std::string::npos) continue;
            auto name = trim(rest.substr(space + 1));
            auto end  = name.find_first_of(" \t");
            if (end != std::string::npos) name = name.substr(0, end);
            if (!name.empty() && (std::isalpha((unsigned char)name[0]) != 0 || name[0] == '_')) {
                definedHere.insert(name);
            }
        }
        in.clear();
        in.seekg(0);
    }

    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] != '#') continue;

        auto firstSpace = trimmed.find_first_of(" \t");
        if (firstSpace == std::string::npos) continue;
        auto directive = lower(trimmed.substr(1, firstSpace - 1));
        auto rest      = trim(trimmed.substr(firstSpace + 1));

        if (directive == "include") {
            // 两种写法都要认：#include "x.sc" 和 #include <framework/main.sh>
            // （Bedrock 的材质普遍用尖括号 + project.json 里的 include_search_paths）。
            std::string relative;
            auto        angle = rest.find('<');
            auto        end   = angle == std::string::npos ? std::string::npos : rest.find('>', angle + 1);
            if (angle != std::string::npos && end != std::string::npos) {
                relative = rest.substr(angle + 1, end - angle - 1);
            } else {
                auto quote = rest.find('"');
                auto close = quote == std::string::npos ? std::string::npos : rest.find('"', quote + 1);
                if (quote != std::string::npos && close != std::string::npos) {
                    relative = rest.substr(quote + 1, close - quote - 1);
                }
            }
            if (!relative.empty()) {
                std::vector<std::filesystem::path> candidates;
                candidates.push_back(file.parent_path() / relative);
                for (auto const& includePath : includePaths) candidates.push_back(includePath / relative);
                for (auto const& candidate : candidates) {
                    if (std::filesystem::is_regular_file(candidate, ec)) {
                        scanFile(candidate, includePaths, macros, visited);
                        break;
                    }
                }
            }
            continue;
        }

        if (directive != "if" && directive != "ifdef" && directive != "ifndef" && directive != "elif") continue;
        if (directive == "ifndef") {
            // 配套的 `#ifndef NAME` + `#define NAME`（SourcePatcher 给默认值套的守卫）
            // 也不是依赖：真正决定产物的是宏的"使用"，不是它的默认值声明。
            auto names = identifiersIn(rest);
            if (names.size() == 1 && definedHere.count(names.front()) != 0) continue;
        }
        for (auto const& identifier : identifiersIn(rest)) {
            if (identifier == "defined" || identifier == "true" || identifier == "false") continue;
            macros.insert(identifier);
        }
    }

    // 上面只看了预处理行，会漏掉"在代码里当数值用"的宏
    // （`float wave = NL_PLANTS_WAVE * windStrength;`）。
    // 漏掉的后果是依赖图里没有"这个宏 -> 哪些材质"，affectedMaterials() 查不到
    // 就保守地返回全部材质 —— 表现为"改一个参数要重编全部材质"。
    // 这里把源码里出现的自定义宏一并记上（只记全大写 + 下划线、长度 >= 4 的，
    // 避免把 vec4 / gl_Position 之类的内建名收进来）。
    in.clear();
    in.seekg(0);
    while (std::getline(in, line)) {
        auto code = line.substr(0, line.find("//"));
        // `#define NAME <body>`：NAME 是定义，body 里出现的宏仍然是依赖。
        auto trimmedCode = trim(code);
        if (!trimmedCode.empty() && trimmedCode[0] == '#') {
            auto after = trim(trimmedCode.substr(1));
            if (after.rfind("define", 0) == 0) {
                auto space = after.find_first_of(" \t");
                if (space != std::string::npos) {
                    auto name = trim(after.substr(space + 1));
                    auto end  = name.find_first_of(" \t");
                    auto token = end == std::string::npos ? name : name.substr(0, end);
                    auto pos   = code.find(token);
                    if (pos != std::string::npos) code = code.substr(pos + token.size());
                }
            } else if (after.rfind("ifndef", 0) == 0 || after.rfind("ifdef", 0) == 0) {
                // 保护默认值用的 `#ifndef NAME` / `#ifdef NAME` 也不是依赖。
                auto space = after.find_first_of(" \t");
                if (space != std::string::npos) {
                    auto name = trim(after.substr(space + 1));
                    if (definedHere.count(name) != 0) code.clear();
                }
            } else if (after.rfind("undef", 0) == 0) {
                // `#undef NAME` 也不是对宏值的使用（config.h 里 NO_WAVE 那类的兜底改写法）。
                code.clear();
            }
        }
        for (auto const& identifier : identifiersIn(code)) {
            if (identifier.size() < 4) continue;
            if (identifier == "defined" || identifier == "true" || identifier == "false") continue;
            bool allCaps = true;
            for (char c : identifier) {
                if (c == '_' || (c >= '0' && c <= '9')) continue;
                if (c >= 'A' && c <= 'Z') continue;
                allCaps = false;
                break;
            }
            if (!allCaps) continue;
            macros.insert(identifier);
        }
    }
}

} // namespace

MacroDependencies scanMacroDependencies(std::filesystem::path const& packDir) {
    MacroDependencies result;
    std::error_code   ec;

    std::filesystem::path materialsRoot = packDir / "materials";
    if (!std::filesystem::is_directory(materialsRoot, ec)) materialsRoot = packDir;
    if (!std::filesystem::is_directory(materialsRoot, ec)) {
        result.conservative = true;
        return result;
    }

    auto includePaths = readIncludePaths(packDir);
    for (auto const& entry : std::filesystem::directory_iterator(materialsRoot, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto material = entry.path().filename().string();
        if (material.empty() || material[0] == '.' || material[0] == '_') continue;

        bool hasSource = false;
        for (auto const& nested : std::filesystem::recursive_directory_iterator(entry.path(), ec)) {
            if (nested.is_regular_file(ec) && isShaderSource(nested.path())) {
                hasSource = true;
                break;
            }
        }
        if (!hasSource) continue;

        result.materials.push_back(material);
        std::set<std::filesystem::path> visited;
        std::set<std::string>           macros;
        for (auto const& nested : std::filesystem::recursive_directory_iterator(entry.path(), ec)) {
            if (nested.is_regular_file(ec)) scanFile(nested.path(), includePaths, macros, visited);
        }
        // 注意：这里**不要**在空集时塞一个 "*"。
        // "用了零个参数"（只 include bgfx_shader.sh 的材质）
        // 和 "解析不出来、只能保守处理" 是两件事：前者应该稳定命中缓存，
        // 后者才需要退化成"依赖全部参数"。真正的未知集合用 `"*"` 显式标记
        // （目前只有整棵材质树都扫不到源码时才会出现，那种情况已在上面 return）。
        result.materialMacros[material] = macros;
        for (auto const& macro : macros) result.macroToMaterials[macro].insert(material);
    }

    if (result.materials.empty()) result.conservative = true;
    return result;
}

std::vector<std::string> affectedMaterials(
    MacroDependencies const&          dependencies,
    std::vector<std::string> const&    changedMacros
) {
    if (dependencies.conservative || changedMacros.empty()) return dependencies.materials;

    std::set<std::string> affected;
    for (auto const& macro : changedMacros) {
        auto it = dependencies.macroToMaterials.find(macro);
        if (it == dependencies.macroToMaterials.end()) {
            return dependencies.materials;
        }
        affected.insert(it->second.begin(), it->second.end());
    }
    return {affected.begin(), affected.end()};
}

std::string materialSourceFingerprint(std::filesystem::path const& packDir, std::string const& material) {
    auto root = packDir / "materials" / material;
    if (!std::filesystem::is_directory(root)) root = packDir / material;
    std::vector<std::string> extensions = {
        ".sc", ".glsl", ".h", ".inc", ".frag", ".vert", ".comp", ".json", ".toml",
    };
    std::string fingerprint = "material:" + hashDirectory(root, extensions);
    for (auto const& shared : {"src", "include", "vanilla"}) {
        fingerprint += "\n";
        fingerprint += shared;
        fingerprint += ":";
        fingerprint += hashDirectory(packDir / shared, extensions);
    }
    fingerprint += "\nproject:";
    fingerprint += hashFile(packDir / "project.json");
    return hashText(fingerprint);
}

std::string materialPattern(std::filesystem::path const& packDir, std::string const& material) {
    std::error_code ec;
    auto candidate = packDir / "materials" / material;
    if (!std::filesystem::is_directory(candidate, ec)) candidate = packDir / material;
    auto absolute = std::filesystem::absolute(candidate, ec);
    return (ec ? candidate : absolute).string();
}

} // namespace renderdragon_shader_loader
