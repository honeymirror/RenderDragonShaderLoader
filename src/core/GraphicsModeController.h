#pragma once

#include <cstdint>
#include <filesystem>

#include "core/ShaderPack.h"

namespace renderdragon_shader_loader {

// 光影启用时自动切换“图形模式”（必须在客户端线程调用）：
//   前向光影 -> 花式（Fancy）
//   延迟光影 -> 灵动视效（Vibrant Visuals / Advanced）
// 关闭光影 -> 花式（Fancy）
class GraphicsModeController {
public:
    // 本次 applyForRenderMode 真的切换了图形模式。模式切换本身会完整重建材质，
    // 调用方必须依赖这次自然加载，不能再 queueReload 一次。
    static constexpr std::uint64_t kGraphicsModeChanged = 2;

    static bool applyForPack(ShaderPack const& pack);
    static bool applyForRenderMode(ShaderRenderMode mode, std::uint64_t* pendingFlags = nullptr);
    static bool applyForward(std::uint64_t* pendingFlags = nullptr);
    // Advanced -> Fancy 时引擎会复用缓存的 Fancy 材质，不一定重新读文件。
    // 等模式切换稳定后再执行一次安全的 Forward 刷新；空目录表示恢复原版。
    static void queueForwardReloadAfterModeChange(std::filesystem::path outputDir);

    // 通过 IAdvancedGraphicsOptions 执行 Advanced -> Simple -> Advanced。
    // 不调用 requestMaterialReload；让引擎自己的模式切换监听器完整重建资源。
    static void queueDeferredPackSwitch(std::filesystem::path outputDir);
    static void cancelDeferredPackSwitch();
};

} // namespace renderdragon_shader_loader
