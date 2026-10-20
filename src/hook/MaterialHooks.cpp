#include "hook/MaterialHooks.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Signature.h"
#include "ll/api/mod/NativeMod.h"

#include "mc/deps/minecraft_renderer/framebuilder/FrameBuilder.h"
#include "mc/deps/core/file/PathView.h"

namespace renderdragon_shader_loader {
namespace {

// 沿用 BRD 的 1.26.20 / 1.26.30 模式。1.26.51.01 的 exe 上四个签名都唯一命中
// （requestMaterialReload 0x5E21BA0 / endFrame 0x5E25FC0 / readFile 0x2CB260）。
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

constexpr std::string_view requestMaterialReloadPatterns[] = {
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
// void BgfxFrameBuilder::requestMaterialReload(uintptr_t self)
using RequestMaterialReloadFn = void (*)(uintptr_t);

ReadFileFn                g_readFileOriginal      = nullptr;
EndFrameFn                g_endFrameOriginal      = nullptr;
RequestMaterialReloadFn   g_requestMaterialReload = nullptr;

std::string* readFileDetour(void* self, std::string* result, Core::PathView* path) {
    if (g_readFileOriginal == nullptr) return result;
    std::string* ret = g_readFileOriginal(self, result, path);

    if (ret != nullptr && path != nullptr) {
        // 诊断：把引擎请求过的**所有** *.material.bin 路径记下来（每条首次 + 每 25 次）。
        // 用来回答"引擎到底从哪儿读材质"——之前只看重定向命中，误判成"引擎在用它"。
        {
            std::string_view p = path->getUtf8StringView();
            if (p.size() > 17 && p.find(".material.bin") != std::string_view::npos) {
                static std::map<std::string, int> seen;
                int n = ++seen[std::string(p)];
                if (n == 1 || n % 25 == 0) {
                    ll::mod::NativeMod::current()->getLogger().info(
                        "[material-read] {} ({} bytes, #{} times)",
                        p,
                        ret->size(),
                        n
                    );
                }
            }
        }
        std::string overrideBytes;
        if (MaterialHooks::getInstance().tryOverrideRead(path->getUtf8StringView(), overrideBytes)
            && !overrideBytes.empty()) {
            // 诊断用：记录"哪个材质、被替换了几次"。之前只记前 5 条又分批写日志，
            // 完全看不出引擎在正常渲染时到底读不读我们的文件；现在每条材质的
            // 首次替换 + 之后每 25 次都会写一行。
            static std::map<std::string, int> counts;
            auto const& key = path->getUtf8StringView();
            int         n   = ++counts[std::string(key)];
            if (n == 1 || n % 25 == 0) {
                ll::mod::NativeMod::current()->getLogger().info(
                    "[reload] overrode {} ({} bytes, #{} times)",
                    key,
                    overrideBytes.size(),
                    n
                );
            }
            ret->assign(overrideBytes);
        }
    }
    return ret;
}

void endFrameDetour(uintptr_t self, uintptr_t context) {
    if (g_endFrameOriginal != nullptr) g_endFrameOriginal(self, context);

    if (MaterialHooks::getInstance().consumeRenderingCacheClear()) {
        auto* frameBuilder = reinterpret_cast<mce::framebuilder::FrameBuilder*>(self);
        frameBuilder->clearRenderingResourcesCache();
        ll::mod::NativeMod::current()->getLogger().info(
            "[reload] cleared rendering resources cache"
        );
    }

    std::filesystem::path outputDir;
    if (g_requestMaterialReload != nullptr && MaterialHooks::getInstance().consumeReload(outputDir)) {
        // 26.51 的 0x5E21BA0 是 BgfxFrameBuilder::requestReload：
        // 它只设置 this+0x718，随后由下一帧 endFrame 执行实际重建。
        // 必须等本帧完整提交后再置标；传材质管理器会写到无关对象，
        // 产生"日志说 reloaded、材质却完全没重读"的假成功。
        g_requestMaterialReload(self);
        auto& logger = ll::mod::NativeMod::current()->getLogger();
        if (outputDir.empty()) {
            logger.info("Vanilla material reload queued for next frame");
        } else {
            logger.info("Shader material reload queued for next frame: {}", outputDir);
        }
    }
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
    mOverrideDir     = std::move(dir);
    mRedirectEnabled = !mOverrideDir.empty();
    // 快速连续切换光影时，旧目录的下一帧重载请求不能跨到新模式切换里。
    mPendingDir.clear();
    mReloadPending = false;
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

void MaterialHooks::requestRenderingCacheClear() {
    std::lock_guard lock(mMutex);
    mClearRenderingResources = true;
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

bool MaterialHooks::consumeRenderingCacheClear() {
    std::lock_guard lock(mMutex);
    if (!mClearRenderingResources) return false;
    mClearRenderingResources = false;
    return true;
}

bool MaterialHooks::install() {
    {
        std::lock_guard lock(mMutex);
        if (mInstalled) return true;
    }

    auto& logger = ll::mod::NativeMod::current()->getLogger();

    // requestMaterialReload（不 hook，仅定位用于调用）。
    void* requestTarget =
        resolveFirst(requestMaterialReloadPatterns, std::size(requestMaterialReloadPatterns));
    if (requestTarget != nullptr) {
        g_requestMaterialReload = reinterpret_cast<RequestMaterialReloadFn>(requestTarget);
    } else {
        logger.warn("MaterialHooks: requestMaterialReload signature not found; hot reload disabled");
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
    mReloadAvailable = g_requestMaterialReload != nullptr && g_endFrameOriginal != nullptr;
    return true;
}

void MaterialHooks::uninstall() {
    std::lock_guard lock(mMutex);
    mInstalled = false;
    // hook 的卸载由 LeviLamina 在模组禁用时统一处理；这里只重置状态。
}

} // namespace renderdragon_shader_loader
