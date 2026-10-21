#include "core/GraphicsModeController.h"

#include <chrono>
#include <memory>

#include "core/CompilerService.h"
#include "hook/MaterialHooks.h"
#include "ui/ShaderManagerScreen.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/input/MinecraftInputHandler.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/options/GraphicsMode.h"
#include "mc/options/IAdvancedGraphicsOptions.h"
#include "mc/options/option_types/BoolOption.h"
#include "mc/options/option_types/IntOption.h"
#include "mc/options/option_types/Option.h"
#include "mc/options/option_types/OptionID.h"
#include "mc/options/option_types/OptionType.h"

namespace renderdragon_shader_loader {
namespace {

char const* gameModeName(GraphicsMode mode) {
    switch (mode) {
    case GraphicsMode::Simple:
        return "Simple";
    case GraphicsMode::Fancy:
        return "Fancy";
    case GraphicsMode::Advanced:
        return "Advanced";
    case GraphicsMode::RayTraced:
        return "RayTraced";
    }
    return "Unknown";
}

// OptionID 的数值每个版本都可能变，所以运行时按保存键扫描并缓存。
int cachedOptionId(
    IClientInstance& client,
    std::string_view saveTag,
    OptionType       type,
    int&             slot
) {
    if (slot >= 0) return slot;
    auto& options = client.getOptions();
    for (int id = 0; id < 2048; ++id) {
        auto candidate = options.getIfValid(static_cast<OptionID>(id));
        if (!candidate.has_value() || *candidate == nullptr) continue;
        Option* option = *candidate;
        auto    impl   = option->mImpl.get();
        if (!impl) continue;
        if (impl->mOptionType == type && impl->mSaveTag.get() == saveTag) {
            slot = id;
            return id;
        }
    }
    return -1;
}

bool setBoolOption(IClientInstance& client, std::string_view saveTag, bool value) {
    static int slot = -1;
    int const  id   = cachedOptionId(client, saveTag, OptionType::Boolean, slot);
    if (id < 0) return false;
    auto candidate = client.getOptions().getIfValid(static_cast<OptionID>(id));
    if (!candidate.has_value() || *candidate == nullptr) return false;
    static_cast<BoolOption*>(*candidate)->set(value, true);
    return true;
}

bool setGraphicsModeOption(IClientInstance& client, GraphicsMode mode) {
    static int slot = -1;
    int id = cachedOptionId(client, "graphics_mode", OptionType::Int, slot);
    if (id < 0) id = cachedOptionId(client, "graphics_mode", OptionType::Enum, slot);
    if (id < 0) return false;
    auto candidate = client.getOptions().getIfValid(static_cast<OptionID>(id));
    if (!candidate.has_value() || *candidate == nullptr) return false;
    Option&    option = **candidate;
    OptionType type   = option.mImpl.get()->mOptionType;
    if (type != OptionType::Int && type != OptionType::Enum) return false;
    static_cast<IntOption*>(&option)->set(static_cast<int>(mode), true);
    return true;
}

bool setGraphicsModeViaCurrentScreen(IClientInstance& client, GraphicsMode target) {
    (void)client;
    return ShaderManagerScreen::getInstance().setGraphicsModeInPlace(
        static_cast<int>(target)
    );
}

bool applyGraphicsMode(GraphicsMode target) {
    auto client = ll::service::getClientInstance();
    if (!client) return false;
    auto& logger = ll::mod::NativeMod::current()->getLogger();
    if (client->getOptions().getGraphicsMode() == target) {
        logger.info("[graphics] already in {}", gameModeName(target));
        return true;
    }
    bool const screenModelSet = setGraphicsModeViaCurrentScreen(*client, target);
    bool       advancedSet    = false;
    if (!screenModelSet) {
        auto input = client->getMinecraftInput();
        if (input) {
        input->mAdvancedGraphicsOptions->setCanSwitchGraphicsModeInGame(true);
        input->mAdvancedGraphicsOptions->setGraphicsMode(target);
        advancedSet = true;
        }
    }

    // IAdvancedGraphicsOptions 是引擎真正的模式切换入口。只有拿不到实例时
    // 才退回直接写 option；旧实现只写 option，日志里 Advanced 会读回 Fancy。
    bool const switchAllowed = setBoolOption(*client, "graphics_mode_switch", true);
    bool const optionSet = (screenModelSet || advancedSet)
        ? true
        : setGraphicsModeOption(*client, target);
    logger.info(
        "[graphics] switch {} (switchOption={}, modeOption={}, screenModel={}, advanced={}, now={})",
        gameModeName(target),
        switchAllowed ? "true" : "false",
        optionSet ? "true" : "false",
        screenModelSet ? "true" : "false",
        advancedSet ? "true" : "false",
        gameModeName(client->getOptions().getGraphicsMode())
    );
    return client->getOptions().getGraphicsMode() == target;
}

bool applyGraphicsModeOption(ShaderRenderMode shaderMode) {
    return applyGraphicsMode(
        shaderMode == ShaderRenderMode::Deferred ? GraphicsMode::Advanced : GraphicsMode::Fancy
    );
}

std::shared_ptr<std::uint64_t>& forwardRefreshToken() {
    static std::shared_ptr<std::uint64_t> token;
    return token;
}

std::shared_ptr<std::uint64_t>& deferredPackSwitchToken() {
    static std::shared_ptr<std::uint64_t> token;
    return token;
}

void pumpDeferredPackSwitch(
    std::shared_ptr<std::uint64_t> token,
    std::filesystem::path        outputDir
) {
    if (deferredPackSwitchToken() != token) return;

    auto& hooks = MaterialHooks::getInstance();
    hooks.setOverrideDir(outputDir);
    hooks.queueReload(outputDir);
    ll::mod::NativeMod::current()->getLogger().info(
        "[graphics] Simple-mode Deferred material reload queued: {}",
        outputDir.string()
    );

    ll::thread::ClientThreadExecutor::getDefault().executeAfter(
        [token, outputDir = std::move(outputDir)]() mutable {
            if (deferredPackSwitchToken() != token) return;

            auto& hooks = MaterialHooks::getInstance();
            if (!GraphicsModeController::applyForRenderMode(ShaderRenderMode::Deferred)) {
                hooks.setOverrideDir({});
                deferredPackSwitchToken().reset();
                CompilerService::getInstance().setStatusNotice("Deferred shader switch failed");
                return;
            }
            ll::mod::NativeMod::current()->getLogger().info(
                "[graphics] Deferred pack switch restored Advanced: {}",
                outputDir.string()
            );

            ll::thread::ClientThreadExecutor::getDefault().executeAfter(
                [token] {
                    if (deferredPackSwitchToken() != token) return;
                    MaterialHooks::getInstance().requestRenderingCacheClear();
                    ll::mod::NativeMod::current()->getLogger().info(
                        "[graphics] cleared post-switch Deferred rendering resources"
                    );
                    CompilerService::getInstance().setStatusNotice("Deferred shader switched");
                    deferredPackSwitchToken().reset();
                },
                std::chrono::milliseconds(900)
            );
        },
        std::chrono::milliseconds(800)
    );
}

void pumpForwardRefresh(
    std::shared_ptr<std::uint64_t> token,
    std::filesystem::path        outputDir,
    int                          remaining,
    bool                         settle
) {
    if (forwardRefreshToken() != token) return;

    auto& logger = ll::mod::NativeMod::current()->getLogger();
    auto  client = ll::service::getClientInstance();
    if (!client) {
        forwardRefreshToken().reset();
        return;
    }

    if (!settle) {
        if (client->getOptions().getGraphicsMode() != GraphicsMode::Fancy) {
            if (remaining <= 0) {
                forwardRefreshToken().reset();
                logger.warn("[graphics] Fancy mode did not become ready; forward refresh cancelled");
                return;
            }
            ll::thread::ClientThreadExecutor::getDefault().executeAfter(
                [token, outputDir = std::move(outputDir), remaining]() mutable {
                    pumpForwardRefresh(std::move(token), std::move(outputDir), remaining - 1, false);
                },
                std::chrono::milliseconds(50)
            );
            return;
        }

        // 模式值虽然已经变成 Fancy，但旧 Fancy 管线还在释放/重建。给引擎约 1 秒，
        // 然后再做一次普通 Forward 重载，确保读到的是当前重定向目录（或原版）。
        ll::thread::ClientThreadExecutor::getDefault().executeAfter(
            [token, outputDir = std::move(outputDir)]() mutable {
                pumpForwardRefresh(std::move(token), std::move(outputDir), 0, true);
            },
            std::chrono::milliseconds(1000)
        );
        return;
    }

    auto& hooks = MaterialHooks::getInstance();
    if (hooks.overrideDir() != outputDir) {
        forwardRefreshToken().reset();
        return;
    }

    if (outputDir.empty()) {
        hooks.restoreVanilla();
        logger.info("[graphics] vanilla Forward refresh queued after mode change");
    } else {
        hooks.queueReload(outputDir);
        logger.info("[graphics] Forward refresh queued after mode change: {}", outputDir.string());
    }
    forwardRefreshToken().reset();
}

} // namespace

bool GraphicsModeController::applyForRenderMode(ShaderRenderMode mode, std::uint64_t* pendingFlags) {
    if (pendingFlags != nullptr) *pendingFlags = 0;
    forwardRefreshToken().reset();

    auto client = ll::service::getClientInstance();
    if (!client) return false;

    auto const modeBefore = client->getOptions().getGraphicsMode();
    // 恢复 v0.1.1 的行为：自动切图形模式。走的是视频设置界面同一条
    // graphics_mode 选项路径，不再依赖 IAdvancedGraphicsOptions 实例。
    applyGraphicsModeOption(mode);
    auto const modeAfter = client->getOptions().getGraphicsMode();
    if (modeBefore != modeAfter && pendingFlags != nullptr) {
        *pendingFlags |= kGraphicsModeChanged;
    }

    return true;
}

bool GraphicsModeController::applyForPack(ShaderPack const& pack) {
    return applyForRenderMode(pack.renderMode);
}

bool GraphicsModeController::applyForward(std::uint64_t* pendingFlags) {
    cancelDeferredPackSwitch();
    return applyForRenderMode(ShaderRenderMode::Forward, pendingFlags);
}

void GraphicsModeController::queueForwardReloadAfterModeChange(std::filesystem::path outputDir) {
    cancelDeferredPackSwitch();
    auto token = std::make_shared<std::uint64_t>(0);
    forwardRefreshToken() = token;
    ll::thread::ClientThreadExecutor::getDefault().executeAfter(
        [token, outputDir = std::move(outputDir)]() mutable {
            pumpForwardRefresh(std::move(token), std::move(outputDir), 600, false);
        },
        std::chrono::milliseconds(50)
    );
}

void GraphicsModeController::queueDeferredPackSwitch(std::filesystem::path outputDir) {
    forwardRefreshToken().reset();
    cancelDeferredPackSwitch();

    auto token = std::make_shared<std::uint64_t>(0);
    deferredPackSwitchToken() = token;

    auto& hooks = MaterialHooks::getInstance();
    hooks.setOverrideDir({});
    hooks.requestRenderingCacheClear();
    applyGraphicsMode(GraphicsMode::Simple);
    CompilerService::getInstance().setStatusNotice("Switching Deferred pipeline...");

    ll::thread::ClientThreadExecutor::getDefault().executeAfter(
        [token, outputDir = std::move(outputDir)]() mutable {
            pumpDeferredPackSwitch(std::move(token), std::move(outputDir));
        },
        std::chrono::milliseconds(1200)
    );
}

void GraphicsModeController::cancelDeferredPackSwitch() {
    deferredPackSwitchToken().reset();
}

} // namespace renderdragon_shader_loader
