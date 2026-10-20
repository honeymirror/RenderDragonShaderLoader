// 离线验证 ui::Screen 的键盘导航（不进游戏）：
//   1) 底栏「完成」按上方向键要落到「光影包设置…」（用户报的 bug）
//   2) 方向键能走到右下角「全透明」和左下角「设置」
//   3) 设置页方向键能走到顶栏四个按钮
//   4) 左下角设置页能录面板热键并写回 config
//   5) 传一个 shaderpacks 目录参数时改测 .mcpack 导入（自动改名 + 识别 + 路线判定）
//
// 编译 & 运行（在 RenderDragonShaderLoader/ 目录下）：
//   clang-cl /nologo /std:c++20 /EHsc /utf-8 /I src tools/nav_test.cpp ^
//     src/ui/UiScreen.cpp src/ui/UiSettings.cpp src/ui/UiLayout.cpp ^
//     src/core/ShaderPack.cpp src/core/Hotkey.cpp /Fe:build\nav_test.exe
//   build\nav_test.exe
//   build\nav_test.exe "<装了 *.mcpack 的 shaderpacks 目录>"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "ui/UiScreen.h"

using namespace renderdragon_shader_loader::ui;
namespace ui = renderdragon_shader_loader::ui;

namespace {

struct NullCanvas final : Canvas {
    void fill(float, float, float, float, Color, float) override {}
    void text(float, float, float, std::string const&, Color, Align, float) override {}
    float textWidth(std::string const& value, float size) const override {
        return (float)value.size() * 6.f * size;
    }
    void flush() override {}
};

struct Log {
    std::vector<std::string> lines;
    std::vector<std::pair<std::string, std::string>> writes;

    void add(std::string const& s) { lines.push_back(s); }
    bool has(std::string const& s) const {
        for (auto const& l : lines) {
            if (l == s) return true;
        }
        return false;
    }
};

int gFailures = 0;

void check(bool ok, std::string const& what) {
    std::printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", what.c_str());
    if (!ok) ++gFailures;
}

constexpr int kUp = 0x26, kDown = 0x28, kLeft = 0x25, kRight = 0x27, kEnter = 0x0D, kEsc = 0x1B;

} // namespace

// .mcpack 导入：扫描一个装着 <名字>.mcpack 的 shaderpacks 目录，
// 应该认出这个包、并把文件自动改名成 .zip。
int runPackTest(int argc, char** argv) {
    std::filesystem::path const packsRoot = argv[1];
    std::filesystem::path const generated  = packsRoot.parent_path() / "generated-test";
    renderdragon_shader_loader::ShaderPackStore::getInstance().scan(packsRoot, generated);
    auto const& packs = renderdragon_shader_loader::ShaderPackStore::getInstance().packs();
    std::printf("found %d pack(s)\n", (int)packs.size());
    for (auto const& p : packs) {
        std::printf(
            "  name=%s bins=%d renderMode=%d\n",
            p.name.c_str(),
            (int)std::count_if(
                std::filesystem::directory_iterator(p.binsDir),
                std::filesystem::directory_iterator(),
                [](std::filesystem::directory_entry const& e) {
                    return e.path().filename().string().ends_with(".material.bin");
                }
            ),
            (int)p.renderMode
        );
    }
    check(packs.size() == 1 && !packs.empty() && packs[0].name == "MyPack", ".mcpack 被当成光影包识别");
    bool renamed = std::filesystem::exists(packsRoot / "MyPack.zip") &&
                   !std::filesystem::exists(packsRoot / "MyPack.mcpack");
    check(renamed, ".mcpack 自动改名成 .zip");
    if (!packs.empty()) {
        check(packs[0].renderMode == renderdragon_shader_loader::ShaderRenderMode::Forward,
              "只有 RenderChunk 产物 -> 判成前向（花式）");
    }
    std::printf("%s (%d failure(s))\n", gFailures == 0 ? "ALL PASS" : "HAS FAILURES", gFailures);
    return gFailures == 0 ? 0 : 1;
}

namespace {

bool overlaps(ui::Rect const& a, ui::Rect const& b) {
    return a.valid() && b.valid() && a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

} // namespace

// 版式回归：任何 GUI 尺寸下，底栏按钮 / 内容行 / 设置页内容栏都不许压到左右两个角落按钮
// （左下「设置」、右下「全透明」）。标度 100% 时 GUI 宽度小，老版式会把「光影包设置…」
// 和「完成」压到右下角按钮上。
int runLayoutTest() {
    float const widths[]  = {320.f, 360.f, 400.f, 427.f, 460.f, 480.f, 520.f, 640.f, 854.f, 1280.f, 1920.f};
    float const heights[] = {240.f, 300.f, 360.f, 480.f, 720.f, 1080.f};

    for (float w : widths) {
        for (float h : heights) {
            std::string const tag = std::to_string((int)w) + "x" + std::to_string((int)h);
            ui::Layout const  L   = ui::buildLayout(w, h);

            check(L.previewButton.valid(), "右下角「全透明」按钮存在 @" + tag);
            check(L.settingsButton.valid(), "左下角「设置」按钮存在 @" + tag);

            for (int i = 0; i < L.buttonCount; ++i) {
                check(
                    !overlaps(L.buttons[i], L.previewButton),
                    "底栏按钮 " + std::to_string(i) + " 不压右下角「全透明」 @" + tag
                );
                check(
                    !overlaps(L.buttons[i], L.settingsButton),
                    "底栏按钮 " + std::to_string(i) + " 不压左下角「设置」 @" + tag
                );
            }
            for (int r = 0; r < L.rowCount; ++r) {
                check(!overlaps(L.rows[r], L.previewButton), "内容行 " + std::to_string(r) + " 不压「全透明」 @" + tag);
                check(!overlaps(L.rows[r], L.settingsButton), "内容行 " + std::to_string(r) + " 不压「设置」 @" + tag);
            }

            // 设置页 / 模组设置页（同一套 buildSettingsLayout）。
            ui::SettingsModel const model = ui::makeDemoSettings();
            std::vector<int> const  view  = ui::settingsTopLevelView(model);
            ui::SettingsLayout const S =
                ui::buildSettingsLayout(w, h, model, view, ui::SettingPageStyle::ButtonGrid, -1, 0.f);
            check(!overlaps(S.content, L.previewButton), "设置页内容栏不压「全透明」 @" + tag);
            check(!overlaps(S.content, L.settingsButton), "设置页内容栏不压「设置」 @" + tag);
            check(!overlaps(S.viewport, L.previewButton), "设置页可视区不压「全透明」 @" + tag);
            check(!overlaps(S.headerBack, L.settingsButton), "设置页「返回」不压左下角「设置」 @" + tag);
            for (auto const& box : S.slots) {
                if (box.row_.valid()) {
                    check(!overlaps(box.row_, L.previewButton), "设置项不压「全透明」 @" + tag);
                    check(!overlaps(box.row_, L.settingsButton), "设置项不压「设置」 @" + tag);
                }
            }
        }
    }

    std::printf("%s (%d failure(s))\n", gFailures == 0 ? "ALL PASS" : "HAS FAILURES", gFailures);
    return gFailures == 0 ? 0 : 1;
}

#include <filesystem>

int runPackTest(int argc, char** argv);
int runLayoutTest();

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--layout") return runLayoutTest();
    if (argc > 1) return runPackTest(argc, argv);

    Log log;
    NullCanvas      canvas;
    ui::Screen      screen;
    ui::ScreenHost  host;

    host.translate = [](std::string_view key) { return std::string(key); };
    host.log       = [&log](std::string const& line) { log.add(line); };
    host.openFolder = [&log]() { log.add("openFolder"); };
    host.rescanPacks = []() {};
    host.compiling   = []() { return false; };
    host.readSetting = [](std::string const& key) -> std::string {
        if (key == "uiKey") return "G";
        if (key == "forceVibrantVisuals") return "ON";
        if (key == "autoLoadLastShader") return "ON";
        if (key == "graphicsModeOverride") return "0";
        return {};
    };
    host.writeSetting = [&log](std::string const& key, std::string const& value) {
        log.writes.emplace_back(key, value);
        log.add("writeSetting:" + key + "=" + value);
        return true;
    };
    host.importSettings = [&log](std::string const&, std::string&) {
        log.add("importSettings");
        return false;
    };
    screen.setHost(std::move(host));

    float const W = 854.f, H = 480.f;
    screen.open();
    screen.render(canvas, W, H);
    check(screen.mode() == ui::Screen::Mode::List, "启动后停在列表页");

    auto press = [&screen](int key) { screen.handleKey(key); };
    auto dump = [&screen](char const* tag) {
        std::printf("       [%s] zone=%d index=%d mode=%d\n", tag, (int)screen.focusZone(), screen.focusIndex(),
                    (int)screen.mode());
    };
    auto clickButton = [&screen, &canvas, W, H](int index) {
        screen.render(canvas, W, H);
        auto const& L  = screen.layout();
        float const cx = (L.buttons[index].x0 + L.buttons[index].x1) / 2.f;
        float const cy = (L.buttons[index].y0 + L.buttons[index].y1) / 2.f;
        screen.handleMouseButton(ui::MouseButton::Left, cx, cy, true);
        screen.handleMouseButton(ui::MouseButton::Left, cx, cy, false);
    };

    // ---- 1) 底栏「完成」按上 -> 正上方的「光影包设置…」 ----
    press(kDown);              // 内容区到底 -> 焦点交给底栏第一个
    dump("down");
    press(kRight);             // 2 -> 3
    dump("right1");
    press(kRight);             // 3 -> 4（完成）
    dump("right2");
    press(kUp);                // 完成 -> 光影包设置…
    dump("up");
    press(kEnter);
    check(screen.mode() == ui::Screen::Mode::Settings, "「完成」上方向键落在「光影包设置…」（进入设置页）");

    // ---- 3) 设置页：第一项按上 -> 顶栏；左右能走到导入/导出/重置 ----
    press(kUp);                // 内容第一项 -> 顶栏
    press(kRight);             // 顶栏 1 -> 2（导入）
    press(kEnter);
    check(log.has("importSettings"), "设置页顶栏「导入」可以用方向键+回车触发");
    press(kDown);              // 顶栏 -> 内容第一项
    press(kUp);                // 回到顶栏（记住上一次的位置）
    press(kLeft);              // 2 -> 1（返回）
    press(kLeft);              // 1 -> 0（返回）
    press(kEnter);
    check(screen.mode() == ui::Screen::Mode::List, "顶栏「返回」回到列表页");

    // ---- 2) 右下角「全透明」 ----
    press(kDown);              // 焦点 -> 底栏
    press(kRight);             // -> 应用
    press(kRight);             // -> 完成
    press(kRight);             // -> 右下角「全透明」
    press(kEnter);
    check(screen.transparentPreview(), "方向键可以走到右下角「全透明」并回车打开");
    press(kEsc);
    check(!screen.transparentPreview(), "Esc 可以退出全透明");

    // ---- 2b) 左下角「设置」 ----
    press(kDown);              // 焦点 -> 底栏
    press(kLeft);              // 底栏最左 -> 左下角「设置」
    press(kEnter);
    check(screen.mode() == ui::Screen::Mode::Preferences, "方向键可以走到左下角「设置」并回车打开");

    // ---- 4) 录热键 + 「应用」写回配置 ----
    press(kEnter);             // 第一项（面板热键）-> 录键
    check(screen.capturingKey(), "回车后面板热键进入录键状态");
    press(0x48);               // 'H'
    check(!screen.capturingKey(), "按键后退出录键状态");
    clickButton(3);            // 底栏「应用」
    bool wroteHotkey = false;
    for (auto const& [k, v] : log.writes) {
        if (k == "uiKey" && v == "H") wroteHotkey = true;
    }
    check(wroteHotkey, "「应用」把录到的面板热键写回配置");

    // ---- 设置页里按 Esc 回列表（鼠标点过底栏，第一下 Esc 先把焦点收回内容区） ----
    press(kEsc);
    press(kEsc);
    check(screen.mode() == ui::Screen::Mode::List, "模组设置页 Esc 回到列表页");

    // ---- 用鼠标进设置页：两次上方向键应落到顶栏「返回」上 ----
    clickButton(1); // 点「光影包设置…」
    check(screen.mode() == ui::Screen::Mode::Settings, "点底栏按钮进入设置页");
    press(kUp);
    dump("up1");
    press(kUp);
    dump("up2");
    check(screen.focusZone() == ui::Screen::Focus::Header && screen.focusIndex() == 0,
          "鼠标进设置页后，两次上方向键落到顶栏「返回」");

    std::printf("\n%s (%d failure(s))\n", gFailures == 0 ? "ALL PASS" : "HAS FAILURES", gFailures);
    std::printf("---- log ----\n");
    for (auto const& l : log.lines) std::printf("  %s\n", l.c_str());
    return gFailures == 0 ? 0 : 1;
}
