#include "core/GraphicsModeController.h"

#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/gui/screens/SceneFactory.h"
#include "mc/options/GraphicsMode.h"
#include "mc/options/IAdvancedGraphicsOptions.h"
#include "mc/options/option_types/BoolOption.h"
#include "mc/options/option_types/IntOption.h"
#include "mc/options/option_types/Option.h"
#include "mc/options/option_types/OptionID.h"
#include "mc/options/option_types/OptionType.h"

namespace renderdragon_shader_loader {
namespace {

char const* modeName(ShaderRenderMode mode) {
    return mode == ShaderRenderMode::Deferred ? "deferred" : "forward";
}

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

// 从 26.40 exe 的选项名序列实测：GRAPHICS_MODE = 711，GRAPHICS_MODE_SWITCH = 798。
// LeviLamina 的 OptionID 头是空枚举，所以按数值取，再用保存键校验，避免版本偏移点错。
constexpr int kGraphicsModeOptionId       = 711;
constexpr int kGraphicsModeSwitchOptionId = 798;

bool setBoolOption(IClientInstance& client, int id, std::string_view expectedSaveTag, bool value) {
    auto& options = client.getOptions();
    Option& option = *options.get(static_cast<OptionID>(id));

    auto const& saveTag = option.mImpl.get()->mSaveTag.get();
    OptionType const type = option.mImpl.get()->mOptionType;
    if (saveTag != expectedSaveTag || type != OptionType::Boolean) return false;

    static_cast<BoolOption*>(&option)->set(value, true);
    return true;
}

bool setGraphicsModeOption(IClientInstance& client, GraphicsMode mode) {
    auto& options = client.getOptions();

    Option& option = *options.get(static_cast<OptionID>(kGraphicsModeOptionId));
    auto const& saveTag = option.mImpl.get()->mSaveTag.get();
    OptionType const type = option.mImpl.get()->mOptionType;
    if (saveTag != "graphics_mode") return false;

    if (type != OptionType::Int && type != OptionType::Enum) return false;

    static_cast<IntOption*>(&option)->set(static_cast<int>(mode), true);
    return true;
}

void setGraphicsModeOnClientThread(GraphicsMode mode, ShaderRenderMode shaderMode) {
    auto& logger = ll::mod::NativeMod::current()->getLogger();

    auto client = ll::service::getClientInstance();
    if (!client) {
        logger.warn("[graphics] cannot switch to {}: client instance unavailable", gameModeName(mode));
        return;
    }

    auto& advanced = *client->getSceneFactory().mAdvancedGraphicsOptions;
    bool const canSwitchBefore = advanced.canSwitchGraphicsModeInGame();
    bool const switchOptionSet =
        setBoolOption(*client, kGraphicsModeSwitchOptionId, "graphics_mode_switch", true);
    logger.info(
        "[graphics] flags: canSwitch={} switchOptionSet={} vibrantAllowed={} deferredResources={} "
        "deferredLighting={} pbrRequired={} perfCompatible={}",
        canSwitchBefore,
        switchOptionSet,
        advanced.isVibrantVisualsAllowed(),
        advanced.areDeferredShadingAndResourcesAvailable(),
        advanced.isDeferredLightingModelAvailable(),
        advanced.requiresPBRResources(),
        advanced.isPlatformDeferredPerformanceCompatible()
    );
    // 视频设置界面会在允许用户改图形模式时把这个开关放开；模组需要在加载光影时
    // 做同样的事，否则 setGraphicsMode 会被游戏自身的运行时保护拒绝。
    advanced.setCanSwitchGraphicsModeInGame(true);
    bool const canSwitchAfter = advanced.canSwitchGraphicsModeInGame();
    advanced.setGraphicsMode(mode);

    // 运行时守卫通常会拒绝 setGraphicsMode；这时直接改真正的 graphics_mode 选项。
    // 这正是视频设置界面的数据源，Option::set 会通知观察者重配渲染模式。
    bool usedOptionFallback = false;
    if (client->getOptions().getGraphicsMode() != mode) {
        usedOptionFallback = setGraphicsModeOption(*client, mode);
    }

    logger.info(
        "[graphics] shader mode={} -> game graphics mode={} (canSwitch {} -> {}, optionFallback={}, now={})",
        modeName(shaderMode),
        gameModeName(mode),
        canSwitchBefore ? "true" : "false",
        canSwitchAfter ? "true" : "false",
        usedOptionFallback ? "true" : "false",
        gameModeName(client->getOptions().getGraphicsMode())
    );
}

} // namespace

void GraphicsModeController::applyForRenderMode(ShaderRenderMode mode) {
    auto const target = mode == ShaderRenderMode::Deferred ? GraphicsMode::Advanced : GraphicsMode::Fancy;
    setGraphicsModeOnClientThread(target, mode);
}

void GraphicsModeController::applyForPack(ShaderPack const& pack) {
    applyForRenderMode(pack.renderMode);
}

void GraphicsModeController::applyForward() {
    applyForRenderMode(ShaderRenderMode::Forward);
}

} // namespace renderdragon_shader_loader
