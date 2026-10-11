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
#include "core/ShaderPack.h"
#include "hook/DragDropHooks.h"
#include "hook/MaterialHooks.h"
#include "ui/ShaderManagerScreen.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/GuiData.h"
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

namespace {
Config config;

// 面板热键：0 = 没绑。
int gHotkeyVk = 0;

// 把配置里的热键名解析成 Windows 虚拟键码。
// 返回 VK 码；0 = 显式不绑（NONE/OFF/-）；-1 = 名字不认识。
int parseHotkey(std::string const& raw) {
    std::string s;
    for (char ch : raw) {
        if (!std::isspace((unsigned char)ch)) s.push_back((char)std::toupper((unsigned char)ch));
    }
    if (s.empty() || s == "NONE" || s == "OFF" || s == "-" || s == "DISABLED") return 0;

    if (s.size() == 1) {
        char const c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; // VK_A..Z / VK_0..9 就是字符本身
        switch (c) {
        case '`':
        case '~': return VK_OEM_3;
        case '-': return VK_OEM_MINUS;
        case '=': return VK_OEM_PLUS;
        case '[': return VK_OEM_4;
        case ']': return VK_OEM_6;
        case ';': return VK_OEM_1;
        case '\'': return VK_OEM_7;
        case ',': return VK_OEM_COMMA;
        case '.': return VK_OEM_PERIOD;
        case '/': return VK_OEM_2;
        case '\\': return VK_OEM_5;
        default: return -1;
        }
    }

    if (s.size() >= 2 && s[0] == 'F') {
        int  n        = 0;
        bool allDigit = true;
        for (std::size_t i = 1; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') {
                allDigit = false;
                break;
            }
            n = n * 10 + (s[i] - '0');
        }
        if (allDigit && n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }

    struct Named {
        char const* name;
        int         vk;
    };
    static constexpr Named named[] = {
        {"SPACE", VK_SPACE},       {"TAB", VK_TAB},
        {"ENTER", VK_RETURN},      {"RETURN", VK_RETURN},
        {"ESC", VK_ESCAPE},        {"ESCAPE", VK_ESCAPE},
        {"BACKSPACE", VK_BACK},    {"INSERT", VK_INSERT},
        {"DELETE", VK_DELETE},     {"DEL", VK_DELETE},
        {"HOME", VK_HOME},         {"END", VK_END},
        {"PGUP", VK_PRIOR},        {"PAGEUP", VK_PRIOR},
        {"PGDN", VK_NEXT},         {"PAGEDOWN", VK_NEXT},
        {"UP", VK_UP},             {"DOWN", VK_DOWN},
        {"LEFT", VK_LEFT},         {"RIGHT", VK_RIGHT},
        {"SHIFT", VK_SHIFT},       {"CTRL", VK_CONTROL},
        {"ALT", VK_MENU},          {"CAPSLOCK", VK_CAPITAL},
        {"NUMPAD0", VK_NUMPAD0},   {"NUMPAD1", VK_NUMPAD1},
        {"NUMPAD2", VK_NUMPAD2},   {"NUMPAD3", VK_NUMPAD3},
        {"NUMPAD4", VK_NUMPAD4},   {"NUMPAD5", VK_NUMPAD5},
        {"NUMPAD6", VK_NUMPAD6},   {"NUMPAD7", VK_NUMPAD7},
        {"NUMPAD8", VK_NUMPAD8},   {"NUMPAD9", VK_NUMPAD9},
    };
    for (auto const& entry : named) {
        if (s == entry.name) return entry.vk;
    }
    return -1;
}

// 把配置里的名字规范化后存回去（字母转大写、去空白）。
std::string normalizeHotkeyName(std::string const& raw) {
    std::string s;
    for (char ch : raw) {
        if (!std::isspace((unsigned char)ch)) s.push_back((char)std::toupper((unsigned char)ch));
    }
    return s;
}

void refreshHotkey() { gHotkeyVk = std::max(parseHotkey(config.uiKey), 0); }

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
    if (parseHotkey(config.uiKey) < 0) {
        logger.warn("Unknown uiKey '{}'; falling back to G", config.uiKey);
        config.uiKey = "G";
        ll::config::saveConfig(config, configPath);
    }
    refreshHotkey();
    logger.info("UI hotkey: {} (vk={})", config.uiKey, gHotkeyVk);

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

    // 注意：这里**故意不自动挂载**任何光影包。默认状态是"光影：已禁用"（原版材质），
    // 由玩家打开面板后自己选一个包再点「应用/完成」才挂上（对齐 Iris 的初始态）。
    // 早先这里会把第一个有 .bin 的包直接 setOverrideDir 挂上去，和"默认不开"是冲突的。
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
    // 让游戏窗口接受外部拖入的 *.zip / 已解压光影包目录。
    installDragDropHook();
    // 卡死诊断看门狗（独立线程，渲染回调停摆 5 秒写 data/ui-hang.log）。
    startHangWatchdog();

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

        if (gHotkeyVk != 0 && key == gHotkeyVk) { // 只在按下沿切换，键盘自动重复不再触发。
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
                    if (parseHotkey(params.key) < 0) {
                        output.error(
                            "Unknown key '{}'. Use A-Z, 0-9, F1-F12, a name like SPACE/TAB/UP, or NONE",
                            params.key
                        );
                        return;
                    }
                    config.uiKey = normalizeHotkeyName(params.key);
                    refreshHotkey();
                    auto const path = RenderDragonShaderLoader::getInstance().getSelf().getConfigDir() / "config.json";
                    if (!ll::config::saveConfig(config, path)) {
                        output.error("Failed to save config: {}", path.string());
                        return;
                    }
                    if (gHotkeyVk == 0) output.success("Panel hotkey disabled (use /shader ui to open)");
                    else output.success("Panel hotkey set to {}", config.uiKey);
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
                output.success("Rolling back to vanilla materials (redirect disabled)");
            });

        }
    );

    return true;
}

bool RenderDragonShaderLoader::disable() {
    stopHangWatchdog();
    uninstallDragDropHook();
    uninstallNativeUiSuppressHooks();
    MaterialHooks::getInstance().uninstall();
    getSelf().getLogger().info("RenderDragonShaderLoader disabled");
    return true;
}

} // namespace renderdragon_shader_loader

LL_REGISTER_MOD(
    renderdragon_shader_loader::RenderDragonShaderLoader,
    renderdragon_shader_loader::RenderDragonShaderLoader::getInstance()
);
