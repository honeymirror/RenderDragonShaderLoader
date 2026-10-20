#include "core/CompileCache.h"

#include <algorithm>
#include <fstream>

#include "core/Hashing.h"

namespace renderdragon_shader_loader {
namespace {

std::string sanitize(std::string value) {
    for (auto& c : value) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>'
            || c == '|') {
            c = '_';
        }
    }
    return value;
}

} // namespace

std::string compileCacheKey(CacheKeyInputs const& inputs) {
    std::string joined = inputs.material;
    joined += '\n';
    joined += inputs.platform;
    joined += '\n';
    joined += inputs.profile;
    joined += '\n';
    joined += inputs.macroValues;
    joined += '\n';
    joined += inputs.sourceFingerprint;
    joined += '\n';
    joined += inputs.compilerVersion;
    return hashText(joined);
}

std::filesystem::path compileCacheEntry(
    std::filesystem::path const& cacheRoot,
    std::string const&           material,
    std::string const&           key
) {
    return cacheRoot / sanitize(material) / (key + ".material.bin");
}

bool lookupCompileCache(
    std::filesystem::path const& cacheRoot,
    std::string const&           material,
    std::string const&           key,
    std::filesystem::path const& destinationDir
) {
    auto source = compileCacheEntry(cacheRoot, material, key);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) return false;
    std::filesystem::create_directories(destinationDir, ec);
    std::filesystem::copy_file(
        source,
        destinationDir / (sanitize(material) + ".material.bin"),
        std::filesystem::copy_options::overwrite_existing,
        ec
    );
    return !ec;
}

bool storeCompileCache(
    std::filesystem::path const& cacheRoot,
    std::string const&           material,
    std::string const&           key,
    std::filesystem::path const& compiledFile
) {
    if (!std::filesystem::is_regular_file(compiledFile)) return false;
    auto destination = compileCacheEntry(cacheRoot, material, key);
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::filesystem::copy_file(
        compiledFile,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        ec
    );
    return !ec;
}

void clearCompileCache(std::filesystem::path const& cacheRoot) {
    std::error_code ec;
    std::filesystem::remove_all(cacheRoot, ec);
}

} // namespace renderdragon_shader_loader
