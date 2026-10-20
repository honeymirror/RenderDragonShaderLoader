#include "core/VibrantVisuals.h"

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Signature.h"
#include "ll/api/mod/NativeMod.h"

namespace renderdragon_shader_loader {
namespace {

// 1.26.20 的 BRD 模式（= BetterRenderDragon 的 forceEnableVibrantVisuals 钩的那个判定函数）。
// 在 1.26.51.01 的 Minecraft.Windows.exe 上用 tools/find_sig.py 实测唯一命中：
//   file=0x2118C90  section=.text  rva=0x2119890  va=0x142119890  count=1
constexpr std::string_view vibrantVisualsPatterns[] = {
    "80 79 ? ? 74 ? 31 C0 C3 0F B6 41 ? C3 CC CC 88 51",
    // 1.21.100（旧版备用）
    "80 79 ? ? 75 ? 80 79 ? ? 74 ? B0 01 C3 32 C0 C3",
};

// bool isVibrantVisualsUsable(void* self)
using VibrantVisualsFn = bool (*)(void*);

VibrantVisualsFn g_original = nullptr;
std::atomic_bool  g_forced{false};
std::atomic_bool  g_installed{false};
std::mutex        g_errorMutex;
std::string       g_lastError;

void setError(std::string message) {
    std::lock_guard lock(g_errorMutex);
    g_lastError = std::move(message);
}

// BRD 原文：
//   *(bool*)(a1 + 48) = 1; *(bool*)(a1 + 49) = 1; *(bool*)(a1 + 50) = 1;
//   *(bool*)(a1 + 51) = 0; *(bool*)(a1 + 52) = 1;  return true;
// 这几个 bool 就是"支持/可用"的能力位，全部置上后游戏会认为本机可以使用灵动视效。
bool vibrantVisualsDetour(void* self) {
    if (g_forced.load(std::memory_order_relaxed) && self != nullptr) {
        auto* bytes = reinterpret_cast<unsigned char*>(self);
        bytes[48]   = 1;
        bytes[49]   = 1;
        bytes[50]   = 1;
        bytes[51]   = 0;
        bytes[52]   = 1;
        return true;
    }
    return g_original != nullptr ? g_original(self) : false;
}

void* resolveFirst(std::string_view const* patterns, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (void* address = ll::memory::Signature::parse(patterns[i]).view().resolve(true)) return address;
    }
    return nullptr;
}

} // namespace

bool installVibrantVisualsHook() {
    if (g_installed.load()) return true;

    void* target = resolveFirst(vibrantVisualsPatterns, std::size(vibrantVisualsPatterns));
    if (target == nullptr) {
        setError("vibrant visuals signature not found");
        ll::mod::NativeMod::current()->getLogger().warn(
            "VibrantVisuals: signature not found; force-enable disabled"
        );
        return false;
    }

    ll::memory::hook(
        target,
        ll::memory::toFuncPtr(&vibrantVisualsDetour),
        reinterpret_cast<ll::memory::FuncPtr*>(&g_original),
        ll::memory::HookPriority::Normal
    );
    if (g_original == nullptr) {
        setError("vibrant visuals hook failed");
        ll::mod::NativeMod::current()->getLogger().error("VibrantVisuals: hook failed");
        return false;
    }

    setError({});
    g_installed.store(true);
    ll::mod::NativeMod::current()->getLogger().info("VibrantVisuals: hook installed");
    return true;
}

void uninstallVibrantVisualsHook() {
    // hook 的卸载由 LeviLamina 在模组禁用时统一处理；这里只把状态放开，
    // 让判定函数回到原逻辑（hook 本身留着不会改变行为）。
    g_forced.store(false);
}

void setForceVibrantVisuals(bool enabled) { g_forced.store(enabled); }

bool forceVibrantVisuals() { return g_forced.load(); }

bool vibrantVisualsHookAvailable() { return g_installed.load(); }

std::string vibrantVisualsLastError() {
    std::lock_guard lock(g_errorMutex);
    return g_lastError;
}

} // namespace renderdragon_shader_loader
