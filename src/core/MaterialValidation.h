#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace renderdragon_shader_loader {

// 1.26.51 的原版材质全是格式版本 26（1.26.40 是 25），编译产物按目标版本写 26。
// 读取时同时接受 25/26：旧包、旧缓存（v25）与游戏自带快照（v26）都能过校验。
inline constexpr std::uint32_t kMaterialFormatVersion    = 26;
inline constexpr std::uint32_t kMaterialMinFormatVersion = 25;
inline constexpr std::uint32_t kMaterialMagic            = 0x0A11DA1Au;

bool readMaterialHeader(std::filesystem::path const& file, std::uint32_t& magic, std::uint32_t& version);
bool validateMaterialFile(std::filesystem::path const& file, std::string* error = nullptr);
bool validateMaterialDir(std::filesystem::path const& dir, std::string* error = nullptr);
// Validate material binaries and, when present, the material list in meta.json.
bool validateCompiledOutput(std::filesystem::path const& dir, std::string* error = nullptr);

} // namespace renderdragon_shader_loader
