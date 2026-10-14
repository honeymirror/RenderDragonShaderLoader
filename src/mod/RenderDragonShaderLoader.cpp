#include "mod/RenderDragonShaderLoader.h"

#include <algorithm>
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
#include "ll/api/thread/ClientThreadExecutor.h"

#include "core/GraphicsModeController.h"
#include "core/Hotkey.h"
#include "core/ShaderPack.h"
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
struct KeyParams {
    std::string key;
};

// 命令参数结构体必须有外部链接（boost::pfr 反射需要），不能放进匿名命名空间。
struct ToggleParams {
    std::string state;
};

namespace {
Config config;

// 面板热键：0 = 没绑。
int gHotkeyVk = 0;

// 热键名 <-> 虚拟键码的映射在 core/Hotkey.{h,cpp}（界面设置页也要用同一张表）。
void refreshHotkey() { gHotkeyVk = std::max(hotkeyVkFromName(config.uiKey), 0); }

// 数一个目录里有多少 *.material.bin。
std::size_t countMaterialBins(std::filesystem::path const& dir) {
    std::error_code ec;
    std::size_t     count = 0;
    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename().string().ends_with(".material.bin")) ++count;
    }
    return count;
}

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
    store.scan(getPacksDir(), getGeneratedDir());

    logger.info("Scanned shaderpacks {} , found {} pack(s)", getPacksDir(), store.packs().size());
    for (auto const& pack : store.packs()) {
        logger.info("  - {} ({} parameter(s))", pack.name, pack.parameters.size());
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

void RenderDragonShaderLoader::rememberShaderSelection(std::string const& packName, bool enabled) {
    if (config.lastShaderPack == packName && config.lastShaderEnabled == enabled) return;
    updateConfig([&](Config& cfg) {
        cfg.lastShaderPack    = packName;
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
    if (countMaterialBins(pack->binsDir) == 0) {
        logger.warn("Last shader pack '{}' has no compiled output; not restoring", config.lastShaderPack);
        return;
    }

    auto const mode = applyRenderModeOverride(pack->renderMode, config.graphicsModeOverride);
    auto const dir  = pack->binsDir;
    auto const name = pack->name;
    ll::thread::ClientThreadExecutor::getDefault().execute([dir, mode, name] {
        GraphicsModeController::applyForRenderMode(mode);
        MaterialHooks::getInstance().queueReload(dir);
        ll::mod::NativeMod::current()->getLogger().info("Restored last shader pack '{}'", name);
    });
    ShaderManagerScreen::getInstance().setAppliedShaderPack(config.lastShaderPack, true);
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
    for (auto const& dir : {getPacksDir(), getGeneratedDir()}) {
        if (!std::filesystem::is_directory(dir, ec) && !std::filesystem::create_directories(dir, ec)) {
            logger.warn("Cannot create directory {}: {}", dir, ec.message());
        }
    }

    rescan();

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
                    if (!RenderDragonShaderLoader::getInstance().setPanelHotkey(params.key)) {
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

            handle.overload().text("rollback").execute([](CommandOrigin const&, CommandOutput& output) {
                auto& hooks = MaterialHooks::getInstance();
                if (!hooks.reloadAvailable()) {
                    output.error("Hot reload unavailable (reloadMaterial signature not found)");
                    return;
                }
                ll::thread::ClientThreadExecutor::getDefault().execute([] {
                    GraphicsModeController::applyForward();
                    MaterialHooks::getInstance().restoreVanilla();
                });
                RenderDragonShaderLoader::getInstance().rememberShaderSelection("", false);
                output.success("Rolling back to vanilla materials (redirect disabled)");
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
    getSelf().getLogger().info("RenderDragonShaderLoader disabled");
    return true;
}

} // namespace renderdragon_shader_loader

LL_REGISTER_MOD(
    renderdragon_shader_loader::RenderDragonShaderLoader,
    renderdragon_shader_loader::RenderDragonShaderLoader::getInstance()
);
