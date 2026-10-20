#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace renderdragon_shader_loader {

struct MacroDependencies {
    std::vector<std::string>                     materials;
    std::map<std::string, std::set<std::string>> macroToMaterials;
    // material -> 该材质真正引用到的宏名。用于给单个材质算"只跟它有关的"缓存键，
    // 这样改一个参数时其它材质仍能命中缓存。
    std::map<std::string, std::set<std::string>> materialMacros;
    bool                                         conservative = false;
};

// Scan material source directories and include closures. If a source cannot be
// parsed reliably, that material is marked as depending on every known macro.
MacroDependencies scanMacroDependencies(std::filesystem::path const& packDir);

// Return the materials affected by any of the supplied changed macros. Unknown
// macros conservatively affect every material.
std::vector<std::string> affectedMaterials(
    MacroDependencies const& dependencies,
    std::vector<std::string> const& changedMacros
);

// Hash of the source files that make up one material (includes are included).
std::string materialSourceFingerprint(std::filesystem::path const& packDir, std::string const& material);

// Resolve a material name to the relative pattern expected by lazurite.
std::string materialPattern(std::filesystem::path const& packDir, std::string const& material);

} // namespace renderdragon_shader_loader
