#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace renderdragon_shader_loader {

// 把源码里形如 `#define NAME ...` 的行包成：
//
//   #ifndef NAME
//   #define NAME ...          // 原来的默认值
//   #endif
//
// 这样命令行注入的 `-d NAME=值` 才能生效。
// BGFX shaderc 的预处理器对「命令行已定义 + 源码再 #define」是**硬报错**
// （Redefining defined variable），不是被覆盖，所以必须让源码里的定义变成"仅当未定义时才生效"。
//
// 只处理 names 里列出的宏；已经处于 `#ifndef NAME` / `#if !defined(NAME)` 保护下的不动。
// 只改内存中的文本，不碰磁盘。返回是否有改动。
bool patchDefines(std::string& source, std::vector<std::string> const& names);

// 便捷重载：单个宏名。
bool patchDefines(std::string& source, std::string_view name);

} // namespace renderdragon_shader_loader
