#pragma once

#include <cstddef>
#include <filesystem>

namespace renderdragon_shader_loader {

// 探测游戏安装目录下的 data/renderer/materials。
// 以模组自身的绝对路径为锚点（<版本目录>/mods/<模组名> -> <版本目录>/data/renderer/materials），
// 因为它比 getModsRoot() 更可靠（后者在客户端里可能是相对路径）。找不到返回空路径。
//
// 注意：这个目录只作为 lazurite 的 merge_source「来源」，编译时才硬链接进暂存目录，
// 不会往 shaders/ 里塞东西（shaders/ 只放着色器包）。
std::filesystem::path detectGameMaterialsDir(std::filesystem::path const& modDir);

} // namespace renderdragon_shader_loader
