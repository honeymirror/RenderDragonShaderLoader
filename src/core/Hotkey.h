#pragma once

#include <string>

namespace renderdragon_shader_loader {

// 面板热键的「名字 <-> Windows 虚拟键码」映射。
// 刻意不包含 <Windows.h>：用到的 VK_* 本来就只是一组数字常量，写成数字后 ui/ 与
// 核心层可以共用同一张表（UiScreen 也要用 hotkeyNameFromVk 来显示/捕获键位）。

// hotkeyVkFromName 的两个特殊返回值。
inline constexpr int kHotkeyNone    = 0;  // 显式不绑（NONE / OFF / - / 空）
inline constexpr int kHotkeyUnknown = -1; // 名字不认识

// 规范化：去掉空白、字母转大写（"n one" -> "NONE"）。
std::string normalizeHotkeyName(std::string const& raw);

// 名字 -> VK 码；kHotkeyNone / kHotkeyUnknown 见上。
int hotkeyVkFromName(std::string const& raw);

// VK 码 -> 规范名字（可以再传回 hotkeyVkFromName）；不认识返回空串。
std::string hotkeyNameFromVk(int vk);

} // namespace renderdragon_shader_loader
