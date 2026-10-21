#include "mod/RenderDragonShaderLoader.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <Windows.h>

#include "ll/api/Config.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/input/KeyRegistry.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "core/CompilerService.h"
#include "core/CompileCache.h"
#include "core/CompileStages.h"
#include "core/GraphicsModeController.h"
#include "core/Hotkey.h"
#include "core/MaterialValidation.h"
#include "core/ShaderPack.h"
#include "core/VanillaBackup.h"
#include "core/VanillaSource.h"
#include "core/VibrantVisuals.h"
#include "hook/DragDropHooks.h"
#include "hook/MaterialHooks.h"
#include "ui/ShaderManagerScreen.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/gui/screens/AbstractScene.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/input/KeyboardManager.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"

#include "mod/Config.h"

namespace renderdragon_shader_loader {

// 命令参数结构体必须有外部链接（boost::pfr 反射需要），不能放进匿名命名空间。
struct CompileParams {
    std::string pack;
};

// 命令参数结构体必须有外部链接（boost::pfr 反射需要），不能放进匿名命名空间。
struct KeyParams {
    std::string key;
};

// 命令参数结构体必须有外部链接（boost::pfr 反射需要），不能放进匿名命名空间。
struct ToggleParams {
    std::string state;
};

struct PresetParams {
    std::string pack;
    std::string preset;
};

struct PresetRenameParams {
    std::string pack;
    std::string preset;
    std::string id;
};

namespace {
Config config;

// 面板热键：0 = 没绑。
int gHotkeyVk = 0;

// 热键名 <-> 虚拟键码的映射在 core/Hotkey.{h,cpp}（界面设置页也要用同一张表）。
void refreshHotkey() { gHotkeyVk = std::max(hotkeyVkFromName(config.uiKey), 0); }

// 游戏此刻是不是"正在打字"。这些情况下面板热键必须让路：那个键本来该打进输入框，
// 既不能顺手把面板弹出来，也不能被我们 cancel 掉（否则玩家会打不出这个字符）。
//   ① 文本框已经拿到键盘（触屏/手柄的软键盘，以及部分平台的文本编辑）；
//   ② 顶层屏幕是聊天栏 / 告示牌 / 铁砧改名 / 书 / 命令方块这些要打字的界面
//      （游戏的屏幕名形如 `chat.chat_screen`、`sign.sign_screen` …）。
bool isGameTyping() {
    auto client = ll::service::getClientInstance();
    if (!client) return false;

    auto& keyboard = client->getKeyboardManager();
    if (keyboard.isKeyboardActive() || keyboard.isKeyboardEnabled()) return true;

    auto         sceneStack = client->getCurrentSceneStack();
    ISceneStack* stack      = sceneStack.operator->();
    if (stack == nullptr) return false;
    auto* scene = stack->getTopScene();
    if (scene == nullptr) return false;

    static constexpr std::string_view kTypingScreens[] = {
        "chat_screen",
        "sign_screen",
        "anvil_screen",
        "book_screen",
        "command_block_screen",
    };
    for (auto const needle : kTypingScreens) {
        if (scene->containsScreenNameSubstring(needle)) return true;
    }
    return false;
}

std::string_view statusName(CompileStatus status) {
    switch (status) {
    case CompileStatus::Idle:
        return "idle";
    case CompileStatus::Queued:
        return "queued";
    case CompileStatus::Running:
        return "running";
    case CompileStatus::Succeeded:
        return "succeeded";
    case CompileStatus::Failed:
        return "failed";
    case CompileStatus::Cancelled:
        return "cancelled";
    }
    return "unknown";
}
} // namespace

RenderDragonShaderLoader& RenderDragonShaderLoader::getInstance() {
    static RenderDragonShaderLoader instance;
    return instance;
}

std::filesystem::path RenderDragonShaderLoader::getPacksDir() const {
    // 玩家放包的地方（对标 OptiFine/Iris 的 shaderpacks 目录）：*.zip 包 + *.txt 选项配置。
    return getSelf().getModDir() / config.dataDir / config.shaderPacksDir;
}

std::filesystem::path RenderDragonShaderLoader::getGeneratedDir() const {
    // 运行时产生的文件：zip 解压出的工作副本、编译好的 *.material.bin 等。
    return getSelf().getModDir() / config.dataDir / config.generatedDir;
}

void RenderDragonShaderLoader::rescan() {
    auto& logger = getSelf().getLogger();
    auto& store  = ShaderPackStore::getInstance();
    store.scan(
        getPacksDir(),
        getGeneratedDir(),
        getSelf().getModDir() / config.dataDir / config.presetsDir
    );

    logger.info("Scanned shaderpacks {} , found {} pack(s)", getPacksDir(), store.packs().size());
    for (auto const& pack : store.packs()) {
        // 顺带打一两个参数名：options.json 只写了 macro 时，参数名必须回退到 macro，
        // 否则设置文件会写成 "=0.2" 这种没有键名的行，玩家的修改就读不回来。
        if (pack.parameters.empty()) {
            logger.info("  - {} (0 parameter(s))", pack.name);
        } else {
            logger.info(
                "  - {} ({} parameter(s), first='{}', preset='{}', bins='{}')",
                pack.name,
                pack.parameters.size(),
                pack.parameters.front().name,
                pack.currentPreset,
                pack.binsDir.string()
            );
        }
        for (auto const& param : pack.parameters) {
            logger.debug("      {} [{}] = {}", param.name, (int)param.type, param.current);
        }
    }
}

Config const& RenderDragonShaderLoader::getConfig() const { return config; }

bool RenderDragonShaderLoader::updateConfig(std::function<void(Config&)> const& mutate) {
    if (mutate) mutate(config);
    auto const configPath = getSelf().getConfigDir() / "config.json";
    if (!ll::config::saveConfig(config, configPath)) {
        getSelf().getLogger().error("Cannot save config to {}", configPath);
        return false;
    }
    return true;
}

void RenderDragonShaderLoader::rememberShaderSelection(
    std::string const& packName,
    bool               enabled,
    std::string const& presetName
) {
    if (config.lastShaderPack == packName && config.lastShaderEnabled == enabled
        && config.lastShaderPreset == presetName) {
        return;
    }
    updateConfig([&](Config& cfg) {
        cfg.lastShaderPack    = packName;
        cfg.lastShaderPreset  = presetName;
        cfg.lastShaderEnabled = enabled;
    });
    getSelf().getLogger().info(
        "Remembered shader selection: pack='{}' enabled={}",
        packName.empty() ? "(vanilla)" : packName,
        enabled
    );
}

bool RenderDragonShaderLoader::setPanelHotkey(std::string const& name) {
    if (hotkeyVkFromName(name) < 0) return false;
    if (!updateConfig([&](Config& cfg) { cfg.uiKey = normalizeHotkeyName(name); })) return false;
    refreshHotkey();
    getSelf().getLogger().info("Panel hotkey set to '{}' (vk={})", config.uiKey, gHotkeyVk);
    return true;
}

// 启动时把上次用的光影挂回去（用户要求：重启游戏后不用手动再点一遍）。
// 只认"上次退出时真的开着、包还在、也有编译产物"这三种条件同时成立的情况。
void RenderDragonShaderLoader::restoreLastShaderOnStart() {
    auto& logger = getSelf().getLogger();
    if (!config.autoLoadLastShader) {
        logger.info("Auto-load last shader pack is disabled");
        return;
    }
    if (!config.lastShaderEnabled || config.lastShaderPack.empty()) {
        logger.info("No last shader pack to restore; starting with vanilla materials");
        return;
    }

    auto const* pack = ShaderPackStore::getInstance().find(config.lastShaderPack);
    if (pack == nullptr) {
        logger.warn("Last shader pack '{}' is no longer installed", config.lastShaderPack);
        return;
    }
    if (!config.lastShaderPreset.empty()) {
        ShaderPackStore::getInstance().selectPreset(pack->name, config.lastShaderPreset);
        pack = ShaderPackStore::getInstance().find(config.lastShaderPack);
    }
    std::string validationError;
    if (compiledOutputNeedsRebuild(pack->binsDir) || !validateCompiledOutput(pack->binsDir, &validationError)) {
        logger.warn("Last shader pack '{}' failed output validation: {}", config.lastShaderPack, validationError);
        return;
    }
    auto const mode  = applyRenderModeOverride(pack->renderMode, config.graphicsModeOverride);
    auto const dir   = pack->binsDir;
    auto const name  = pack->name;
    // 必须同步设置：ClientThreadExecutor 往往晚于引擎开始读材质，
    // 结果只有一部分材质被覆盖（实测 54 个里只盖到 20 多个）。
    MaterialHooks::getInstance().setOverrideDir(dir);
    ll::thread::ClientThreadExecutor::getDefault().execute([mode, name] {
        std::uint64_t pending = 0;
        GraphicsModeController::applyForRenderMode(mode, &pending);
        ll::mod::NativeMod::current()->getLogger().info(
            "Restored last shader pack '{}' ({}); materials will load with the world",
            name,
            mode == ShaderRenderMode::Deferred ? "deferred" : "forward"
        );
    });
    ShaderManagerScreen::getInstance().setAppliedShaderPack(config.lastShaderPack, true);
}

bool RenderDragonShaderLoader::compilePack(std::string const& name, std::string& error) {
    auto const* pack = ShaderPackStore::getInstance().ensureWritablePreset(name);
    if (pack == nullptr) {
        error = "no such shader pack: " + name;
        return false;
    }

    std::vector<std::string> macros;
    for (auto const& parameter : pack->parameters) {
        auto macro = toCompileMacro(parameter);
        if (!macro.empty()) macros.push_back(std::move(macro));
    }

    std::vector<UniformPatch> uniformPatches;
    std::string uniformError;
    if (!collectUniformPatches(pack->parameters, uniformPatches, uniformError)) {
        error = uniformError;
        return false;
    }

    CompileOptions options;
    options.macros    = std::move(macros);
    options.uniformPatches = std::move(uniformPatches);
    options.profiles  = pack->profiles;
    options.outputDir = pack->binsDir;
    options.cacheDir  = pack->cacheDir;
    options.packName  = pack->name;
    options.preset    = pack->currentPreset;
    options.platform  = "Direct3D_SM65";
    options.backend   = pack->manifest.backend.empty() ? config.backend : pack->manifest.backend;
    options.stagedPublish = shouldStageColorCompile(options.macros)
                         && !compiledOutputNeedsRebuild(pack->binsDir);
    options.stageSettleMs = pack->renderMode == ShaderRenderMode::Deferred ? 3000 : 600;

    if (!CompilerService::getInstance().compile(pack->sourceDir, std::move(options))) {
        error = CompilerService::getInstance().lastMessage();
        if (error.empty()) error = "compile refused";
        return false;
    }
    // 记住这次用的选项值，对标 OptiFine/Iris 的 shaderpacks/<包>.txt。
    ShaderPackStore::saveSettings(*pack);
    return true;
}

bool RenderDragonShaderLoader::usePack(std::string const& name, std::string& error) {
    auto const* pack = ShaderPackStore::getInstance().find(name);
    if (pack == nullptr) {
        error = "no such shader pack: " + name;
        return false;
    }
    return usePackAt(name, pack->binsDir, error);
}

bool RenderDragonShaderLoader::usePackAt(
    std::string const&           name,
    std::filesystem::path const& binsDir,
    std::string&                 error
) {
    auto const* pack = ShaderPackStore::getInstance().find(name);
    if (pack == nullptr) {
        error = "no such shader pack: " + name;
        return false;
    }
    auto const targetDir = binsDir.empty() ? pack->binsDir : binsDir;
    std::string validationError;
    if (compiledOutputNeedsRebuild(targetDir) || !validateCompiledOutput(targetDir, &validationError)) {
        error = validationError.empty() ? (name + " has no compiled .material.bin yet") : validationError;
        if (compiledOutputNeedsRebuild(targetDir)) {
            error = name + " compiled output is stale; apply it again to recompile";
        }
        return false;
    }
    if (pack->renderMode == ShaderRenderMode::Deferred) {
        auto const dir = targetDir;
        ll::thread::ClientThreadExecutor::getDefault().execute([dir] {
            GraphicsModeController::queueDeferredPackSwitch(dir);
        });
        CompilerService::getInstance().setStatusNotice("Switching Deferred pipeline...");
        getSelf().getLogger().info(
            "[graphics] Deferred pack '{}' engine switch queued",
            name
        );
        return true;
    }

    auto packCopy = *pack;
    packCopy.binsDir = targetDir;
    ll::thread::ClientThreadExecutor::getDefault().execute([packCopy = std::move(packCopy)]() mutable {
        auto& hooks = MaterialHooks::getInstance();
        hooks.requestRenderingCacheClear();
        hooks.setOverrideDir(packCopy.binsDir);
        std::uint64_t pending = 0;
        if (!GraphicsModeController::applyForRenderMode(packCopy.renderMode, &pending)) {
            CompilerService::getInstance().setStatusNotice(
                "Deferred shader could not load (Vibrant Visuals unavailable)"
            );
            return;
        }
        if (pending & GraphicsModeController::kGraphicsModeChanged) {
            if (packCopy.renderMode == ShaderRenderMode::Forward) {
                GraphicsModeController::queueForwardReloadAfterModeChange(packCopy.binsDir);
            }
            return;
        }
        MaterialHooks::getInstance().queueReload(packCopy.binsDir);
    });
    return true;
}

bool RenderDragonShaderLoader::usePatchedPack(std::string const& name, std::string& error) {
    auto const* pack = ShaderPackStore::getInstance().find(name);
    if (pack == nullptr) {
        error = "no such shader pack: " + name;
        return false;
    }
    if (pack->renderMode != ShaderRenderMode::Deferred) {
        return usePack(name, error);
    }
    return usePack(name, error);
}

std::filesystem::path RenderDragonShaderLoader::createLiveMaterialSnapshot(
    std::filesystem::path const& sourceDir,
    std::string&                 error
) {
    std::error_code ec;
    if (!std::filesystem::is_directory(sourceDir, ec)) {
        error = "material snapshot source is not a directory: " + sourceDir.string();
        return {};
    }

    auto const liveRoot = sourceDir.parent_path() / ".live";
    auto const stamp    = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()
                          )
                              .count();
    auto destination = liveRoot / std::to_string(stamp);
    int suffix = 1;
    while (std::filesystem::exists(destination, ec)) {
        ec.clear();
        destination = liveRoot / (std::to_string(stamp) + "-" + std::to_string(suffix));
        ++suffix;
        if (suffix > 1000) {
            error = "cannot allocate a unique live material snapshot path";
            return {};
        }
    }
    if (!std::filesystem::create_directories(destination, ec) && ec) {
        error = "cannot create live material snapshot: " + ec.message();
        return {};
    }

    std::size_t copied = 0;
    bool failed = false;
    for (auto const& entry : std::filesystem::directory_iterator(sourceDir, ec)) {
        if (ec) {
            failed = true;
            break;
        }
        if (!entry.is_regular_file(ec)) continue;
        auto target = destination / entry.path().filename();
        std::error_code linkError;
        std::filesystem::create_hard_link(entry.path(), target, linkError);
        if (linkError) {
            std::error_code copyError;
            std::filesystem::copy_file(
                entry.path(),
                target,
                std::filesystem::copy_options::overwrite_existing,
                copyError
            );
            if (copyError) {
                failed = true;
                break;
            }
            ++copied;
        } else {
            ++copied;
        }
    }
    if (failed || copied == 0) {
        error = failed ? "cannot copy a file into the live material snapshot"
                       : "material snapshot has no files";
        std::filesystem::remove_all(destination, ec);
        return {};
    }

    std::vector<std::filesystem::path> snapshots;
    for (auto const& entry : std::filesystem::directory_iterator(liveRoot, ec)) {
        if (entry.is_directory(ec)) snapshots.push_back(entry.path());
    }
    std::sort(snapshots.begin(), snapshots.end());
    while (snapshots.size() > 3) {
        std::filesystem::remove_all(snapshots.front(), ec);
        snapshots.erase(snapshots.begin());
    }
    return destination;
}

bool RenderDragonShaderLoader::restorePack(std::string const& name, std::string& error) {
    auto const* pack = ShaderPackStore::getInstance().find(name);
    if (pack == nullptr) {
        error = "no such shader pack: " + name;
        return false;
    }

    auto const finalDir  = pack->binsDir;
    auto const backupDir = finalDir / ".backup";

    std::error_code ec;
    if (!std::filesystem::is_directory(backupDir, ec)) {
        error = "no backup for " + name + " yet";
        return false;
    }

    std::filesystem::path sourceDir;
    std::vector<std::filesystem::path> snapshots;
    for (auto const& entry : std::filesystem::directory_iterator(backupDir, ec)) {
        if (entry.is_directory(ec)) snapshots.push_back(entry.path());
    }
    std::sort(snapshots.begin(), snapshots.end());
    if (!snapshots.empty()) sourceDir = snapshots.back();

    std::size_t restored = 0;
    for (auto const& entry : std::filesystem::directory_iterator(
             sourceDir.empty() ? backupDir : sourceDir,
             ec
         )) {
        if (!entry.is_regular_file(ec)) continue;
        auto fileName = entry.path().filename().string();
        if (!fileName.ends_with(".material.bin")) continue;

        std::error_code stepEc;
        std::filesystem::copy_file(
            entry.path(),
            finalDir / fileName,
            std::filesystem::copy_options::overwrite_existing,
            stepEc
        );
        if (!stepEc) ++restored;
    }

    if (restored == 0) {
        error = "backup for " + name + " has no material.bin";
        return false;
    }

    auto const metaSource = sourceDir.empty() ? backupDir : sourceDir;
    if (std::filesystem::is_regular_file(metaSource / "meta.json", ec)) {
        std::filesystem::copy_file(
            metaSource / "meta.json",
            finalDir / "meta.json",
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
    }

    auto const mode = detectRenderMode(finalDir);
    if (mode == ShaderRenderMode::Deferred) {
        ll::thread::ClientThreadExecutor::getDefault().execute([finalDir] {
            GraphicsModeController::queueDeferredPackSwitch(finalDir);
        });
        CompilerService::getInstance().setStatusNotice("Switching Deferred pipeline...");
        ll::mod::NativeMod::current()->getLogger().info(
            "[graphics] Deferred pack '{}' restored; engine switch queued",
            name
        );
        return true;
    }
    ll::thread::ClientThreadExecutor::getDefault().execute([finalDir, mode] {
        MaterialHooks::getInstance().setOverrideDir(finalDir);
        std::uint64_t pending = 0;
        if (!GraphicsModeController::applyForRenderMode(mode, &pending)) {
            CompilerService::getInstance().setStatusNotice(
                "Deferred shader could not load (Vibrant Visuals unavailable)"
            );
            return;
        }
        if (pending & GraphicsModeController::kGraphicsModeChanged) {
            if (mode == ShaderRenderMode::Forward) {
                GraphicsModeController::queueForwardReloadAfterModeChange(finalDir);
            }
            return;
        }
        MaterialHooks::getInstance().queueReload(finalDir);
    });
    return true;
}

bool RenderDragonShaderLoader::load() {
    auto& logger = getSelf().getLogger();
    logger.info("RenderDragonShaderLoader loading...");

    auto const configPath = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(config, configPath)) {
        logger.warn("Cannot load config from {}, saving defaults", configPath);
        if (!ll::config::saveConfig(config, configPath)) {
            logger.error("Cannot save default config to {}", configPath);
        }
    }
    // 面板热键：默认 G。名字不认识时退回默认值（不会让玩家把面板锁死在外面）。
    if (hotkeyVkFromName(config.uiKey) < 0) {
        logger.warn("Unknown uiKey '{}'; falling back to G", config.uiKey);
        config.uiKey = "G";
        ll::config::saveConfig(config, configPath);
    }
    refreshHotkey();
    logger.info("UI hotkey: {} (vk={})", config.uiKey, gHotkeyVk);
    logger.info(
        "forceVibrantVisuals={} graphicsModeOverride={} autoLoadLastShader={} last='{}'",
        config.forceVibrantVisuals,
        config.graphicsModeOverride,
        config.autoLoadLastShader,
        config.lastShaderPack
    );

    // 语言文件：<modDir>/lang/{zh_CN,en_US}.json
    if (auto res = ll::i18n::getInstance().load(getSelf().getLangDir()); !res) {
        logger.warn("Cannot load language files from {}", getSelf().getLangDir());
    } else {
        logger.info("Loaded language files from {}", getSelf().getLangDir());
    }

    std::error_code ec;
    for (auto const& dir : {
             getPacksDir(),
             getGeneratedDir(),
             getSelf().getModDir() / config.dataDir / config.presetsDir,
         }) {
        if (!std::filesystem::is_directory(dir, ec) && !std::filesystem::create_directories(dir, ec)) {
            logger.warn("Cannot create directory {}: {}", dir, ec.message());
        }
    }

    // 合并源：包自带 vanilla/ 时直接使用；没带的用游戏安装目录
    // data/renderer/materials 作为来源，编译时硬链接进【暂存目录】，
    // 不再往 shaders/ 里塞东西（shaders/ 只放着色器包）。
    std::filesystem::path mergeSource;
    if (!config.vanillaDir.empty()) {
        mergeSource = std::filesystem::path(config.vanillaDir);
        if (mergeSource.is_relative()) mergeSource = getSelf().getModDir() / mergeSource;
    } else {
        mergeSource = detectGameMaterialsDir(getSelf().getModDir());
    }
    if (mergeSource.empty()) {
        logger.warn("no vanilla merge source found; packs without their own vanilla/ folder may fail to build");
    } else {
        logger.info("vanilla merge source: {}", mergeSource);
    }

    // 首次使用即保存原版材质快照，后续“一键还原/导出还原包”都依赖它。
    auto gameMaterials = detectGameMaterialsDir(getSelf().getModDir());
    auto snapshotDir   = getSelf().getModDir() / config.dataDir / "backups" / "vanilla-materials";
    std::string snapshotError;
    if (!gameMaterials.empty() && ensureVanillaSnapshot(gameMaterials, snapshotDir, &snapshotError)) {
        logger.info("vanilla material snapshot ready: {}", snapshotDir);
    } else if (!snapshotError.empty()) {
        logger.warn("vanilla material snapshot failed: {}", snapshotError);
    }

    CompilerSettings settings;
    settings.workingDir      = getSelf().getModDir();
    settings.outputDir       = getGeneratedDir(); // 仅作兜底（正常都传包自己的 binsDir）
    settings.vanillaDir      = mergeSource;
    settings.lazuriteCommand = config.lazuriteCommand;
    settings.lazuriteAlbiteCommand = config.lazuriteAlbiteCommand;
    settings.shadercPath     = config.shadercPath;
    settings.skipValidation  = config.skipValidation;
    settings.enableCompilation = config.enableCompilation;
    settings.materialVersion    = config.materialVersion;
    settings.maxWorkers        = config.maxWorkers;
    settings.backend           = config.backend;
    settings.albitePath        = config.albitePath;
    settings.albiteArgs        = config.albiteArgs;
    settings.timeoutSeconds    = config.timeoutSeconds;
    settings.keepBackupCount   = config.keepBackupCount;
    settings.preferAlbiteProfileRewrite = config.preferAlbiteProfileRewrite;

    // 随模组分发的工具链：<modDir>/toolchain/{lazurite.exe, shaderc.exe}。
    // 只在用户没有显式覆盖时生效。
    auto const toolchain = getSelf().getModDir() / "toolchain";
    if (config.lazuriteCommand == "python -m lazurite") {
        auto bundledLazurite = toolchain / "lazurite.exe";
        if (std::filesystem::is_regular_file(bundledLazurite, ec)) {
            settings.lazuriteCommand = bundledLazurite.string();
        }
    }
    if (settings.shadercPath.empty()) {
        auto bundledShaderc = toolchain / "shaderc.exe";
        if (std::filesystem::is_regular_file(bundledShaderc, ec)) {
            settings.shadercPath = bundledShaderc.string();
        }
    }

    logger.info(
        "Compiler: lazurite='{}' shaderc='{}' enabled={} maxWorkers={}",
        settings.lazuriteCommand,
        settings.shadercPath,
        settings.enableCompilation,
        settings.maxWorkers
    );
    CompilerService::getInstance().configure(std::move(settings));

    // 编译成功后把产物目录挂到材质重定向，并在下一次 endFrame 触发热重载。
    CompilerService::getInstance().setOnFinished(
        [](CompileFinishedInfo const& info) {
            auto const status    = info.status;
            auto const& packDir  = info.packDir;
            auto const& outputDir = info.outputDir;
            auto& logger = ll::mod::NativeMod::current()->getLogger();
            if (status == CompileStatus::Succeeded) {
                if (info.completeOutput) {
                    ShaderManagerScreen::getInstance().setAppliedShaderPack(
                        info.packName,
                        true
                    );
                    RenderDragonShaderLoader::getInstance().rememberShaderSelection(
                        info.packName,
                        true,
                        info.preset
                    );
                }
                RenderDragonShaderLoader::getInstance().markRestartPending();
                auto const mode = detectRenderMode(outputDir);
                auto const packLabel = packDir.filename().string();
                if (mode == ShaderRenderMode::Deferred) {
                    ll::thread::ClientThreadExecutor::getDefault().execute([outputDir] {
                        GraphicsModeController::queueDeferredPackSwitch(outputDir);
                    });
                    CompilerService::getInstance().setStatusNotice(
                        "Deferred shader compiled; switching pipeline"
                    );
                    logger.info(
                        "[graphics] Deferred compile finished for {}; engine switch queued",
                        packLabel
                    );
                } else {
                    ll::thread::ClientThreadExecutor::getDefault().execute([outputDir] {
                        auto& hooks = MaterialHooks::getInstance();
                        hooks.setOverrideDir(outputDir);
                        std::uint64_t pending = 0;
                        if (!GraphicsModeController::applyForRenderMode(
                                ShaderRenderMode::Forward,
                                &pending
                            )) {
                            return;
                        }
                        if (pending & GraphicsModeController::kGraphicsModeChanged) {
                            GraphicsModeController::queueForwardReloadAfterModeChange(outputDir);
                            return;
                        }
                        hooks.queueReload(outputDir);
                    });
                }
                // 把编译日志整段搬进 latest.log。以前只搬最后 8 行，
                // 结果 "cache hit / compile scope / staged ... ms" 这些
                // 关键判定全被截掉，事后无从排查。
                auto        lines = CompilerService::getInstance().log();
                std::size_t from  = lines.size() > 200 ? lines.size() - 200 : 0;
                for (std::size_t i = from; i < lines.size(); ++i) {
                    logger.info("[compile] {}", lines[i]);
                }
                if (mode == ShaderRenderMode::Deferred) {
                    logger.info(
                        "Compile finished for {} ({} material(s)); Deferred engine switch queued",
                        packDir.filename(),
                        info.materialCount
                    );
                } else {
                    logger.info(
                        "Compile finished for {} ({} material(s)); shader reload applied",
                        packDir.filename(),
                        info.materialCount
                    );
                }
            } else {
                CompilerService::getInstance().setStatusNotice(info.message);
                logger.error("Compile failed for {}", packDir.filename());
                // 把编译器最近的输出也写进日志，方便事后从 latest.log 排查
                // （否则只能在游戏里用 /shader log 看）。
                auto        lines = CompilerService::getInstance().log();
                std::size_t from  = lines.size() > 200 ? lines.size() - 200 : 0;
                for (std::size_t i = from; i < lines.size(); ++i) {
                    logger.error("[compile] {}", lines[i]);
                }
            }
        }
    );

    rescan();

    // 关键：把材质重定向**从启动一开始**就打开。
    //
    // 日志证据：引擎是"第一次用到某材质时才去读它"，而且读完就一直在内存里用。
    // 之前的做法是等玩家在面板里应用光影时才 enable+queueReload，
    // 但那时引擎早就把材质读完了 —— 我们替换文件发生得太晚，永远不会生效
    // （典型表现：material-read 只有 #1 次，我们随后的 overrode 引擎再也不读）。
    // 所以启动时如果配置里记着"上次开着某个光影"，就先把重定向挂上，
    // 让引擎的第一次读取就命中我们的产物。
    if (config.autoLoadLastShader && config.lastShaderEnabled && !config.lastShaderPack.empty()) {
        auto const* lastPack = ShaderPackStore::getInstance().find(config.lastShaderPack);
        if (lastPack != nullptr && !config.lastShaderPreset.empty()) {
            ShaderPackStore::getInstance().selectPreset(lastPack->name, config.lastShaderPreset);
            lastPack = ShaderPackStore::getInstance().find(config.lastShaderPack);
        }
        if (lastPack != nullptr) {
            std::error_code ec;
            std::string validationError;
            if (std::filesystem::is_directory(lastPack->binsDir, ec)
                && !compiledOutputNeedsRebuild(lastPack->binsDir)
                && validateCompiledOutput(lastPack->binsDir, &validationError)) {
                MaterialHooks::getInstance().setOverrideDir(lastPack->binsDir);
                logger.info(
                    "[startup] material redirect enabled early -> {}",
                    lastPack->binsDir.string()
                );
            } else {
                logger.warn(
                    "[startup] skipped early redirect for '{}': {}",
                    lastPack->name,
                    validationError.empty() ? "compiled output is stale or incomplete"
                                            : validationError
                );
            }
        }
    }

    // 默认状态是"光影：已禁用"（原版材质），玩家选包后「应用/完成」才会挂上（对齐 Iris）。
    // 例外：配置里记着"上次退出时开着某个包"而且它还装着产物，就在 enable() 里自动挂回去
    // （见 restoreLastShaderOnStart）—— 第一次安装的人配置是空的，仍然从原版材质开始。
    logger.info(
        "shaders start disabled; open the panel ({} / `/shader ui`) to pick a pack",
        config.uiKey.empty() ? "no hotkey" : config.uiKey
    );

    return true;
}

bool RenderDragonShaderLoader::enable() {
    auto& logger = getSelf().getLogger();
    logger.info("RenderDragonShaderLoader enabled");

    auto& hooks = MaterialHooks::getInstance();
    if (hooks.install()) {
        logger.info("Material hooks installed (hot reload available: {})", hooks.reloadAvailable());
    } else {
        logger.error("Material hooks failed: {}", hooks.lastError());
    }

    // 占位屏幕生命周期 hook：只接管它那一趟 UI pass，不再碰 user32 光标和全局绘制函数。
    installNativeUiSuppressHooks();
    // 让游戏窗口接受外部拖入的 *.zip / *.mcpack / 已解压光影包目录。
    installDragDropHook();
    // 卡死诊断看门狗（独立线程，渲染回调停摆 5 秒写 data/ui-hang.log）。
    startHangWatchdog();

    // 强制开启「灵动视效」：有些服务器会把图形模式的判定锁死，这里照 BRD 的做法
    // 钩住判定函数直接置位。关掉的话立刻回到原逻辑。
    if (installVibrantVisualsHook()) {
        setForceVibrantVisuals(config.forceVibrantVisuals);
        logger.info("Force vibrant visuals: {}", config.forceVibrantVisuals);
    } else {
        logger.warn("Force vibrant visuals unavailable: {}", vibrantVisualsLastError());
    }

    // 界面背景压暗档位（0 = 透明看得见世界，1 = 全遮住）。
    ShaderManagerScreen::getInstance().setBackdrop((float)config.uiBackdrop);
    logger.info("UI placeholder hooks installed");

    // 默认状态是“光影：已禁用”，图形模式对应花式。
    ll::thread::ClientThreadExecutor::getDefault().execute([] { GraphicsModeController::applyForward(); });

    auto& bus = ll::event::EventBus::getInstance();

    // 只在"游戏正在渲染我们的占位屏幕"那一趟接管绘制；cancel 掉原生命令对话框，
    // 直接画自己的界面。其余 HUD / 原版屏幕不受影响。
    bus.emplaceListener<ll::event::render::BeforeUIRenderEvent>([](ll::event::render::BeforeUIRenderEvent& event) {
        auto& view   = event.screenView();
        auto& screen = ShaderManagerScreen::getInstance();
        if (!isOurUiPass(view)) return;
        event.cancel();
        screen.setPointer(view.mPointerLocationPrevious->x, view.mPointerLocationPrevious->y);
        screen.render(event.uiRenderContext(), view.mSize->x, view.mSize->y);
    });
    bus.emplaceListener<ll::event::render::AfterUIRenderEvent>([](ll::event::render::AfterUIRenderEvent& event) {
        ShaderManagerScreen::getInstance().afterFrame(event.uiRenderContext());
    });

    // 输入只走游戏事件。界面内部的锁负责跨线程串行；SceneStack 的 push/pop
    // 仍然统一排到客户端线程执行。
    bus.emplaceListener<ll::event::input::MouseInputEvent>([](ll::event::input::MouseInputEvent& event) {
        auto&     screen = ShaderManagerScreen::getInstance();
        int const action = event.actionButtonId();
        if (action == MouseAction::ActionMove || action == MouseAction::ActionMoveRelative) return;
        bool const wheel = action == MouseAction::ActionWheel;
        if (wheel && event.buttonData() == 0) return;
        bool const down = wheel || event.buttonData() == MouseAction::DataDown;

        // MouseInputEvent 坐标是窗口像素；Screen 的布局使用 GUI 单位。
        float invScale = 1.f;
        if (auto client = ll::service::getClientInstance()) {
            invScale = client->getGuiData()->mInvGuiScale;
        }
        if (screen.handleMouse(action, (float)event.x() * invScale, (float)event.y() * invScale, down,
                               wheel ? event.buttonData() : 0)) {
            event.cancel();
        }
    });

    // 界面键盘操作。面板热键直接走原始按键事件（不依赖 KeyRegistry 的键位映射），
    // 默认 G，可用 `/shader key <键名>` 改。
    bus.emplaceListener<ll::event::input::KeyInputEvent>([](ll::event::input::KeyInputEvent& event) {
        auto&      screen = ShaderManagerScreen::getInstance();
        int const  key    = event.keyCode();
        bool const down   = event.isDown();

        // 设置页正在"录键位"时，即使按的是当前热键也要交给界面处理，不能在这里开/关面板。
        if (gHotkeyVk != 0 && key == gHotkeyVk && !screen.capturingKey()) {
            // 正在打字（聊天栏、告示牌、铁砧…）：这个键要让给输入框 —— 不开面板，也不吞事件。
            if (!screen.visible() && isGameTyping()) return;
            // 只在按下沿切换，键盘自动重复不再触发。
            static bool hotkeyHeld = false;
            if (down && !hotkeyHeld) {
                if (screen.visible()) screen.close();
                else screen.open();
            }
            hotkeyHeld = down;
            event.cancel();
            return;
        }

        // 界面内的按键**只在按下沿处理一次**：KeyInputEvent 按下/抬起各来一次，
        // 两边都送进去的话一次物理按键会被执行两次（按一下方向键就走两格、Tab 跳两项）。
        // 抬起沿只负责把事件吃掉，避免漏给游戏。
        if (!screen.visible()) return;
        if (down) screen.handleKey(key);
        event.cancel();
    });

    // 鼠标输入走游戏自己的 MouseInputEvent；事件回调只做轻量复制和排队，
    // 具体命中测试在客户端线程执行。

    bus.emplaceListener<ll::event::command::ClientCommandRegisterEvent>(
        [](ll::event::command::ClientCommandRegisterEvent&) {
            auto& handle = ll::command::CommandRegistrar::getInstance(true).getOrCreateCommand(
                "shader",
                "RenderDragonShaderLoader command",
                CommandPermissionLevel::Any
            );

            handle.overload().text("scan").execute([](CommandOrigin const&, CommandOutput& output) {
                RenderDragonShaderLoader::getInstance().rescan();
                output.success("Rescanned shader packs");
            });

            // 不依赖键位的开面板方式（也可用来判断是键位问题还是渲染问题）。
            handle.overload().text("ui").execute([](CommandOrigin const&, CommandOutput& output) {
                auto& screen = ShaderManagerScreen::getInstance();
                screen.toggle();
                output.success(screen.visible() ? "Shader panel opened" : "Shader panel closed");
            });

            // 面板热键：`/shader key` 看当前值，`/shader key <名字>` 改（NONE = 不绑）。
            handle.overload().text("key").execute([](CommandOrigin const&, CommandOutput& output) {
                output.success("Panel hotkey: {} (change with /shader key <name>)", config.uiKey);
            });

            handle.overload<KeyParams>().text("key").required("key").execute(
                [](CommandOrigin const&, CommandOutput& output, KeyParams const& params) {
                    if (hotkeyVkFromName(params.key) < 0) {
                        output.error(
                            "Unknown key '{}'. Use A-Z, 0-9, F1-F12, a name like SPACE/TAB/UP, or NONE",
                            params.key
                        );
                        return;
                    }
                    config.uiKey = normalizeHotkeyName(params.key);
                    refreshHotkey();
                    if (!RenderDragonShaderLoader::getInstance().updateConfig({})) {
                        output.error("Failed to save config");
                        return;
                    }
                    if (gHotkeyVk == 0) output.success("Panel hotkey disabled (use /shader ui to open)");
                    else output.success("Panel hotkey set to {}", config.uiKey);
                }
            );

            // 强制开启「灵动视效」的开关（有些服务器会把图形模式锁死）。
            // `/shader vibrant` 看状态，`/shader vibrant on|off` 改并立刻生效。
            handle.overload().text("vibrant").execute([](CommandOrigin const&, CommandOutput& output) {
                output.success(
                    "Force vibrant visuals: {} (hook available: {}, use /shader vibrant on|off)",
                    forceVibrantVisuals() ? "on" : "off",
                    vibrantVisualsHookAvailable() ? "yes" : "no"
                );
            });

            handle.overload<ToggleParams>().text("vibrant").required("state").execute(
                [](CommandOrigin const&, CommandOutput& output, ToggleParams const& params) {
                    auto const state = normalizeHotkeyName(params.state); // 复用：去空白 + 大写
                    if (state != "ON" && state != "OFF" && state != "TRUE" && state != "FALSE") {
                        output.error("Usage: /shader vibrant <on|off>");
                        return;
                    }
                    bool const enabled = (state == "ON" || state == "TRUE");
                    if (!RenderDragonShaderLoader::getInstance().updateConfig(
                            [&](Config& cfg) { cfg.forceVibrantVisuals = enabled; }
                        )) {
                        output.error("Failed to save config");
                        return;
                    }
                    setForceVibrantVisuals(enabled);
                    output.success("Force vibrant visuals: {}", enabled ? "on" : "off");
                }
            );

            handle.overload().text("list").execute([](CommandOrigin const&, CommandOutput& output) {
                auto const& packs = ShaderPackStore::getInstance().packs();
                if (packs.empty()) {
                    output.success("No shader packs found");
                    return;
                }
                for (auto const& pack : packs) {
                    output.success(fmt::format("{} - {} parameter(s)", pack.name, pack.parameters.size()));
                }
            });

            handle.overload<CompileParams>().text("compile").required("pack").execute(
                [](CommandOrigin const&, CommandOutput& output, CompileParams const& params) {
                    std::string error;
                    if (RenderDragonShaderLoader::getInstance().compilePack(params.pack, error)) {
                        output.success("Compile started: " + params.pack);
                    } else {
                        output.error(error);
                    }
                }
            );

            handle.overload().text("status").execute([](CommandOrigin const&, CommandOutput& output) {
                auto&       service = CompilerService::getInstance();
                std::string message =
                    fmt::format("Compile status: {} ({})", statusName(service.status()), service.lastMessage());
                output.success(message);
            });

            handle.overload().text("reload").execute([](CommandOrigin const&, CommandOutput& output) {
                auto& hooks   = MaterialHooks::getInstance();
                auto& service = CompilerService::getInstance();
                if (!hooks.reloadAvailable()) {
                    output.error("Hot reload unavailable (requestMaterialReload signature not found)");
                    return;
                }
                auto dir = service.lastOutputDir();
                if (dir.empty()) {
                    output.error("Compile a shader pack first");
                    return;
                }
                if (detectRenderMode(dir) == ShaderRenderMode::Deferred) {
                    output.error("Deferred materials cannot be hot reloaded while Advanced is active");
                    return;
                }
                hooks.queueReload(dir);
                output.success("Shader reload queued: " + dir.string());
            });

            handle.overload().text("rollback").execute([](CommandOrigin const&, CommandOutput& output) {
                auto& hooks = MaterialHooks::getInstance();
                if (!hooks.reloadAvailable()) {
                    output.error("Hot reload unavailable (requestMaterialReload signature not found)");
                    return;
                }
                ll::thread::ClientThreadExecutor::getDefault().execute([] {
                    auto& hooks = MaterialHooks::getInstance();
                    hooks.setOverrideDir({});
                    std::uint64_t pending = 0;
                    if (!GraphicsModeController::applyForward(&pending)) return;
                    if (pending & GraphicsModeController::kGraphicsModeChanged) {
                        GraphicsModeController::queueForwardReloadAfterModeChange({});
                    } else {
                        hooks.restoreVanilla();
                    }
                });
                RenderDragonShaderLoader::getInstance().rememberShaderSelection("", false);
                ll::mod::NativeMod::current()->getLogger().info(
                    "[command] rollback: shader material redirection disabled"
                );
                output.success("Rolling back to vanilla materials (redirect disabled)");
            });

            handle.overload<CompileParams>().text("restore").required("pack").execute(
                [](CommandOrigin const&, CommandOutput& output, CompileParams const& params) {
                    if (params.pack == "vanilla") {
                        output.error(
                            "Direct restore into data/renderer/materials is disabled; "
                            "use a trusted launcher/game backup"
                        );
                        return;
                    }

                    std::string error;
                    if (RenderDragonShaderLoader::getInstance().restorePack(params.pack, error)) {
                        ll::mod::NativeMod::current()->getLogger().info(
                            "[command] restore: package={} restored from its newest backup",
                            params.pack
                        );
                        output.success("Restored previous build of " + params.pack);
                    } else {
                        output.error(error);
                    }
                }
            );

            // 加载某个包已编译好的产物（有 .bin 就用，不编译）。
            handle.overload<CompileParams>().text("use").required("pack").execute(
                [](CommandOrigin const&, CommandOutput& output, CompileParams const& params) {
                    std::string error;
                    if (RenderDragonShaderLoader::getInstance().usePack(params.pack, error)) {
                        RenderDragonShaderLoader::getInstance().rememberShaderSelection(params.pack, true);
                        output.success("Loading compiled materials of " + params.pack);
                    } else {
                        output.error(error);
                    }
                }
            );

            handle.overload().text("log").execute([](CommandOrigin const&, CommandOutput& output) {
                auto lines = CompilerService::getInstance().log();
                if (lines.empty()) {
                    output.success("No compile log yet");
                    return;
                }
                std::size_t start = lines.size() > 15 ? lines.size() - 15 : 0;
                for (std::size_t i = start; i < lines.size(); ++i) {
                    output.success(lines[i]);
                }
            });

            handle.overload<PresetParams>().text("preset").text("use").required("pack").required("preset").execute(
                [](CommandOrigin const&, CommandOutput& output, PresetParams const& params) {
                    if (!ShaderPackStore::getInstance().selectPreset(params.pack, params.preset)) {
                        output.error("No such pack or preset");
                        return;
                    }
                    output.success("Selected preset '{}' for '{}'", params.preset, params.pack);
                }
            );

            handle.overload<PresetParams>().text("preset").text("new").required("pack").required("preset").execute(
                [](CommandOrigin const&, CommandOutput& output, PresetParams const& params) {
                    if (!ShaderPackStore::getInstance().createLocalPreset(params.pack, params.preset)) {
                        output.error("Could not create preset");
                        return;
                    }
                    output.success("Created preset '{}' for '{}'", params.preset, params.pack);
                }
            );

            handle.overload<PresetRenameParams>()
                .text("preset")
                .text("rename")
                .required("pack")
                .required("preset")
                .required("id")
                .execute([](CommandOrigin const&, CommandOutput& output, PresetRenameParams const& params) {
                    if (!ShaderPackStore::getInstance().renameLocalPreset(params.pack, params.preset, params.id)) {
                        output.error("Could not rename preset");
                        return;
                    }
                    output.success("Renamed preset '{}' -> '{}'", params.preset, params.id);
                });

            handle.overload<PresetParams>().text("preset").text("del").required("pack").required("preset").execute(
                [](CommandOrigin const&, CommandOutput& output, PresetParams const& params) {
                    if (!ShaderPackStore::getInstance().deleteLocalPreset(params.pack, params.preset)) {
                        output.error("Could not delete preset (only local presets can be deleted)");
                        return;
                    }
                    output.success("Deleted preset '{}'", params.preset);
                }
            );

            handle.overload<PresetParams>().text("preset").text("export").required("pack").required("preset").execute(
                [](CommandOrigin const&, CommandOutput& output, PresetParams const& params) {
                    auto file = ll::mod::NativeMod::current()->getModDir() / config.dataDir
                              / (params.pack + "-" + params.preset + ".txt");
                    if (!ShaderPackStore::getInstance().exportPreset(params.pack, params.preset, file)) {
                        output.error("Could not export preset");
                        return;
                    }
                    output.success("Exported preset to " + file.string());
                }
            );

            handle.overload<CompileParams>().text("preset").text("list").required("pack").execute(
                [](CommandOrigin const&, CommandOutput& output, CompileParams const& params) {
                    auto const* pack = ShaderPackStore::getInstance().find(params.pack);
                    if (pack == nullptr) {
                        output.error("No such shader pack");
                        return;
                    }
                    for (auto const& preset : pack->presets) {
                        output.success(
                            "{} [{}] {} ({} material(s))",
                            preset.id,
                            preset.origin,
                            preset.compiled ? "compiled" : "not compiled",
                            preset.materialCount
                        );
                    }
                }
            );

            handle.overload().text("cache").text("clear").execute([](CommandOrigin const&, CommandOutput& output) {
                auto root = RenderDragonShaderLoader::getInstance().getGeneratedDir() / "cache";
                clearCompileCache(root);
                output.success("Compile cache cleared: " + root.string());
            });

            handle.overload().text("export").text("restore").execute([](CommandOrigin const&, CommandOutput& output) {
                auto modDir = ll::mod::NativeMod::current()->getModDir();
                auto snapshot = modDir / config.dataDir / "backups" / "vanilla-materials";
                auto zip = modDir / config.dataDir / "restore-vanilla.zip";
                std::string error;
                if (!exportVanillaSnapshot(snapshot, zip, &error)) {
                    output.error(error);
                    return;
                }
                ll::mod::NativeMod::current()->getLogger().info(
                    "[command] export restore: {}",
                    zip.string()
                );
                output.success("Restore package exported: " + zip.string());
            });
        }
    );

    // 上次退出时开着的光影：这里挂回去（必须在 MaterialHooks 装好、监听器注册完之后）。
    restoreLastShaderOnStart();

    return true;
}

bool RenderDragonShaderLoader::disable() {
    stopHangWatchdog();
    uninstallDragDropHook();
    uninstallNativeUiSuppressHooks();
    uninstallVibrantVisualsHook();
    MaterialHooks::getInstance().uninstall();
    CompilerService::getInstance().shutdown();
    getSelf().getLogger().info("RenderDragonShaderLoader disabled");
    return true;
}

} // namespace renderdragon_shader_loader

LL_REGISTER_MOD(
    renderdragon_shader_loader::RenderDragonShaderLoader,
    renderdragon_shader_loader::RenderDragonShaderLoader::getInstance()
);
