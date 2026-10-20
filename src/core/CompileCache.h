#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace renderdragon_shader_loader {

struct CacheKeyInputs {
    std::string material;
    std::string platform;
    std::string profile;
    std::string macroValues;
    std::string sourceFingerprint;
    std::string compilerVersion;
};

std::string compileCacheKey(CacheKeyInputs const& inputs);

std::filesystem::path compileCacheEntry(
    std::filesystem::path const& cacheRoot,
    std::string const&           material,
    std::string const&           key
);

bool lookupCompileCache(
    std::filesystem::path const& cacheRoot,
    std::string const&           material,
    std::string const&           key,
    std::filesystem::path const& destinationDir
);

bool storeCompileCache(
    std::filesystem::path const& cacheRoot,
    std::string const&           material,
    std::string const&           key,
    std::filesystem::path const& compiledFile
);

void clearCompileCache(std::filesystem::path const& cacheRoot);

} // namespace renderdragon_shader_loader
