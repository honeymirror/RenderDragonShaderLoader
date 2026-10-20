#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace renderdragon_shader_loader {

// Stable FNV-1a hashes for cache keys and metadata. Cache keys are not security
// boundaries; a 64-bit content hash is enough to avoid recompiling unchanged inputs.
std::string hashText(std::string_view text);
std::string hashBytes(std::string_view bytes);
std::string hashFile(std::filesystem::path const& file);
std::string hashFiles(std::vector<std::filesystem::path> const& files);
std::string hashDirectory(std::filesystem::path const& root, std::vector<std::string> const& extensions = {});

} // namespace renderdragon_shader_loader
