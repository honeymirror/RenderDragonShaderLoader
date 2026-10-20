#pragma once

#include <string>

namespace renderdragon_shader_loader {

// 强制开启「灵动视效」（Vibrant Visuals / 延迟渲染）。
//
// 有些服务器会让客户端把灵动视效的开关锁死（游戏自己的运行时保护判定"当前不允许"），
// 玩家在视频设置里根本打不开。BetterRenderDragon 的做法是 hook 那个"能不能用灵动视效"
// 的判定函数，直接返回 true 并把能力位全部置位——这里照做：
//
//   函数签名（1.26.20 的 BRD 模式，已在 1.26.51.01 的 exe 上实测唯一命中）：
//     80 79 ? ? 74 ? 31 C0 C3 0F B6 41 ? C3 CC CC 88 51
//
// 生效范围只在 hook 之后；关闭开关会立刻回到原函数。

// 解析签名并挂上 hook。已挂过时直接返回 true。
// 返回 false 表示没定位到函数（换版本了），此时 setForceVibrantVisuals 不起作用。
bool installVibrantVisualsHook();
void uninstallVibrantVisualsHook();

// 打开 / 关闭强制开启（线程安全，可在任何线程调用）。
void setForceVibrantVisuals(bool enabled);
[[nodiscard]] bool forceVibrantVisuals();

// hook 是否已就位（没就位说明这个游戏版本不支持，界面里应该提示）。
[[nodiscard]] bool vibrantVisualsHookAvailable();

// 最近一次安装失败的原因（成功时为空）。
[[nodiscard]] std::string vibrantVisualsLastError();

} // namespace renderdragon_shader_loader
