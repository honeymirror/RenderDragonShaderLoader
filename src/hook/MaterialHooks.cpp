#include "hook/MaterialHooks.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Signature.h"
#include "ll/api/mod/NativeMod.h"

#include "mc/deps/core/file/PathView.h"

namespace renderdragon_shader_loader {
namespace {

// 1.26.40（沿用 BRD 的 1.26.20 / 1.26.30 模式，HANDOFF 第 3.5 节已在 exe 上验证唯一命中）。
constexpr std::string_view readFilePatterns[] = {
    // 1.26.30
    "55 41 57 41 56 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 48 89 D6 41 0F 10 00",
    // 1.26.20
    "55 56 48 83 EC ? 48 8D 6C 24 ? 48 C7 45 ? ? ? ? ? 48 89 D6 48 8D 4D ? 4C 89 45 ? 4C 89 C2 E8 ? ? ? ? 48 8D 55 ? 48 89 F1 E8 ? ? ? ? 48 8B 4D ? E8 ? ? ? ? 48 89 F0 48 83 C4 ? 5E 5D C3 66 66 2E 0F 1F 84 00 ? ? ? ? 48 89 54 24 ? 55 56 48 83 EC ? 48 8D 6A ? 48 8B 4D ? E8 ? ? ? ? 90 48 83 C4 ? 5E 5D C3 55 56",
};

constexpr std::string_view endFramePatterns[] = {
    // 1.26.30
    "55 41 57 41 56 41 55 41 54 56 57 53 B8 ? ? ? ? E8 ? ? ? ? 48 29 C4 48 8D AC 24 ? ? ? ? 44 0F 29 8D ? ? ? ? 44 0F 29 85 ? ? ? ? 0F 29 BD ? ? ? ? 0F 29 B5 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 48 89 95 ? ? ? ? 48 89 8D",
    // 1.26.20
    "55 41 57 41 56 41 55 41 54 56 57 53 B8 ? ? ? ? E8 ? ? ? ? 48 29 C4 48 8D AC 24 ? ? ? ? 66 44 0F 29 95",
};

constexpr std::string_view reloadMaterialPatterns[] = {
    // 1.26.20
    "C6 81 ? ? ? ? ? C3 CC CC CC CC CC CC CC CC 55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 0F 29 75",
};

void* resolveFirst(std::string_view const* patterns, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (void* address = ll::memory::Signature::parse(patterns[i]).view().resolve(true)) {
            return address;
        }
    }
    return nullptr;
}

// std::string* readFile(void* self, std::string* result, Core::PathView* path)
using ReadFileFn = std::string* (*)(void*, std::string*, Core::PathView*);
// void mce::framebuilder::BgfxFrameBuilder::endFrame(uintptr_t self, uintptr_t ctx)
using EndFrameFn = void (*)(uintptr_t, uintptr_t);
// void dragon::materials::BgfxFrameBuilder::reloadMaterial(uintptr_t self)
using ReloadMaterialFn = void (*)(uintptr_t);

ReadFileFn       g_readFileOriginal = nullptr;
EndFrameFn       g_endFrameOriginal = nullptr;
ReloadMaterialFn g_reloadMaterial   = nullptr;

std::string* readFileDetour(void* self, std::string* result, Core::PathView* path) {
    if (g_readFileOriginal == nullptr) return result;
    std::string* ret = g_readFileOriginal(self, result, path);

    if (ret != nullptr && path != nullptr) {
        std::string overrideBytes;
        if (MaterialHooks::getInstance().tryOverrideRead(path->getUtf8StringView(), overrideBytes)
            && !overrideBytes.empty()) {
            // 只记前几条，用来确认"重定向真的生效了"（日志里能看到 overrode ...）。
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 5) {
                ll::mod::NativeMod::current()->getLogger().info(
                    "[reload] overrode {} ({} bytes)",
                    path->getUtf8StringView(),
                    overrideBytes.size()
                );
            }
            ret->assign(overrideBytes);
        }
    }
    return ret;
}

void endFrameDetour(uintptr_t self, uintptr_t context) {
    std::filesystem::path outputDir;
    if (g_reloadMaterial != nullptr && MaterialHooks::getInstance().consumeReload(outputDir)) {
        g_reloadMaterial(self);
        auto& logger = ll::mod::NativeMod::current()->getLogger();
        if (outputDir.empty()) {
            logger.info("Shader materials restored to vanilla");
        } else {
            logger.info("Shader materials reloaded from {}", outputDir);
        }
    }
    if (g_endFrameOriginal != nullptr) g_endFrameOriginal(self, context);
}

} // namespace

MaterialHooks& MaterialHooks::getInstance() {
    static MaterialHooks instance;
    return instance;
}

MaterialHooks::~MaterialHooks() { uninstall(); }

bool MaterialHooks::installed() const {
    std::lock_guard lock(mMutex);
    return mInstalled;
}

bool MaterialHooks::reloadAvailable() const {
    std::lock_guard lock(mMutex);
    return mReloadAvailable;
}

std::string MaterialHooks::lastError() const {
    std::lock_guard lock(mMutex);
    return mLastError;
}

void MaterialHooks::setOverrideDir(std::filesystem::path dir) {
    std::lock_guard lock(mMutex);
    mOverrideDir      = std::move(dir);
    mRedirectEnabled  = !mOverrideDir.empty();
}

std::filesystem::path MaterialHooks::overrideDir() const {
    std::lock_guard lock(mMutex);
    return mOverrideDir;
}

void MaterialHooks::queueReload(std::filesystem::path outputDir) {
    std::lock_guard lock(mMutex);
    mPendingDir      = std::move(outputDir);
    mReloadPending   = true;
    mOverrideDir     = mPendingDir;
    mRedirectEnabled = true;
}

void MaterialHooks::restoreVanilla() {
    std::lock_guard lock(mMutex);
    mPendingDir.clear();
    mReloadPending   = true;
    mOverrideDir.clear();
    mRedirectEnabled = false;
}

bool MaterialHooks::tryOverrideRead(std::string_view path, std::string& out) const {
    std::filesystem::path dir;
    {
        std::lock_guard lock(mMutex);
        if (!mRedirectEnabled || mOverrideDir.empty()) return false;
        dir = mOverrideDir;
    }

    // 路径分隔符可能是 / 也可能是 \（游戏内部两种都出现过），先统一成 / 再判断。
    std::string normalized(path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::string_view const p      = normalized;
    constexpr std::string_view marker = "data/renderer/materials/";
    if (p.find(marker) == std::string_view::npos) return false;
    if (!p.ends_with(".material.bin")) return false;

    auto slashPos = p.find_last_of('/');
    auto name     = p.substr(slashPos == std::string_view::npos ? 0 : slashPos + 1);
    if (name.empty()) return false;

    auto file = dir / std::string(name);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) return false;

    std::ifstream in(file, std::ios::binary);
    if (!in) return false;

    std::string bytes{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>()
    };
    if (bytes.empty()) return false;

    out = std::move(bytes);
    return true;
}

bool MaterialHooks::consumeReload(std::filesystem::path& out) {
    std::lock_guard lock(mMutex);
    if (!mReloadPending) return false;
    out            = mPendingDir;
    mReloadPending = false;
    return true;
}

bool MaterialHooks::install() {
    {
        std::lock_guard lock(mMutex);
        if (mInstalled) return true;
    }

    auto& logger = ll::mod::NativeMod::current()->getLogger();

    // reloadMaterial（不 hook，仅定位用于调用）。
    void* reloadTarget = resolveFirst(reloadMaterialPatterns, std::size(reloadMaterialPatterns));
    if (reloadTarget != nullptr) {
        g_reloadMaterial = reinterpret_cast<ReloadMaterialFn>(reloadTarget);
    } else {
        logger.warn("MaterialHooks: reloadMaterial signature not found; hot reload disabled");
    }

    // readFile hook（材质文件重定向）。
    void* readFileTarget = resolveFirst(readFilePatterns, std::size(readFilePatterns));
    if (readFileTarget == nullptr) {
        std::lock_guard lock(mMutex);
        mLastError = "readFile signature not found";
        logger.error("MaterialHooks: {}", mLastError);
        return false;
    }
    ll::memory::hook(
        readFileTarget,
        ll::memory::toFuncPtr(&readFileDetour),
        reinterpret_cast<ll::memory::FuncPtr*>(&g_readFileOriginal),
        ll::memory::HookPriority::Normal
    );
    if (g_readFileOriginal == nullptr) {
        std::lock_guard lock(mMutex);
        mLastError = "readFile hook failed";
        logger.error("MaterialHooks: {}", mLastError);
        return false;
    }
    logger.info("MaterialHooks: readFile hooked");

    // endFrame hook（在安全时机触发 reloadMaterial）。
    void* endFrameTarget = resolveFirst(endFramePatterns, std::size(endFramePatterns));
    if (endFrameTarget == nullptr) {
        logger.warn("MaterialHooks: endFrame signature not found; reload trigger disabled");
    } else {
        ll::memory::hook(
            endFrameTarget,
            ll::memory::toFuncPtr(&endFrameDetour),
            reinterpret_cast<ll::memory::FuncPtr*>(&g_endFrameOriginal),
            ll::memory::HookPriority::Normal
        );
        if (g_endFrameOriginal == nullptr) {
            logger.warn("MaterialHooks: endFrame hook failed; reload trigger disabled");
        } else {
            logger.info("MaterialHooks: endFrame hooked");
        }
    }

    std::lock_guard lock(mMutex);
    mInstalled       = true;
    mReloadAvailable = g_reloadMaterial != nullptr && g_endFrameOriginal != nullptr;
    return true;
}

void MaterialHooks::uninstall() {
    std::lock_guard lock(mMutex);
    mInstalled = false;
    // hook 的卸载由 LeviLamina 在模组禁用时统一处理；这里只重置状态。
}

} // namespace renderdragon_shader_loader
