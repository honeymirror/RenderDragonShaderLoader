#pragma once

#include "core/ShaderPack.h"

namespace renderdragon_shader_loader {

// 光影启用时自动切换“图形模式”（必须在客户端线程调用）：
//   前向光影 -> 花式（Fancy）
//   延迟光影 -> 灵动视效（Vibrant Visuals / Advanced）
// 关闭光影 -> 花式（Fancy）
class GraphicsModeController {
public:
    static void applyForPack(ShaderPack const& pack);
    static void applyForRenderMode(ShaderRenderMode mode);
    static void applyForward();
};

} // namespace renderdragon_shader_loader
