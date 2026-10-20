// 离线 UI 沙盒：不启动游戏，直接跑布局 + 命中测试 + 焦点遍历。
//
// 编译 & 运行（在 RenderDragonShaderLoader/ 目录下）：
//   clang-cl /nologo /std:c++20 /EHsc /utf-8 /I src tools/ui_sim.cpp ^
//     src/ui/UiLayout.cpp src/ui/UiSettings.cpp src/core/ShaderPack.cpp /Fe:build\ui_sim.exe
//   build\ui_sim.exe "E:\...\mods\RenderDragonShaderLoader\data\shaderpacks" 640 371
//
// 然后在控制台里输命令（每行一条）：
//   列表页： move X Y | click X Y | key <键码> | grid | list
//           （键码：38=上 40=下 37=左 39=右 13=回车 27=Esc）
//   设置页： settings            进入设置页沙盒并打印版式概览
//           sgrid               打印所有设置项的行/列/控件矩形
//           sc X Y              设置页点击（真实 settingsHitTest + 值操作）
//           sk <键码>            设置页按键（38/40=上下选择，37/39=左右，13=激活，219/221=[ ]）
//           ssel                打印当前选中项与值
//   1 / 2 / 3               切换列表页模式；q 退出

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "core/ShaderPack.h"
#include "ui/UiLayout.h"
#include "ui/UiSettings.h"

using namespace renderdragon_shader_loader;
using namespace renderdragon_shader_loader::ui;

namespace {

enum class Mode { List, Settings };
char const* modeName(Mode m) { return m == Mode::List ? "List" : "Settings"; }

struct Sim {
    std::vector<std::string> packs;
    std::vector<int>         paramCounts;
    Layout                   layout;
    Mode                     mode = Mode::List;
    int                      row  = 0;
    int                      buttonFocus = -1;
    float                    mx = -1, my = -1;
    float                    width = 640.f, height = 371.f;

    // 设置页沙盒：直接用 UiSettings 的真实函数（版式 / 命中 / 值操作）。
    SettingsModel    settings = makeDemoSettings();
    SettingsLayout   sLayout;
    std::vector<int> sView   = settingsTopLevelView(settings);
    int              sPage   = -1; // -1 = 顶层分类页（按钮网格），否则是分类项下标
    int              sIndex  = sView.empty() ? -1 : sView.front();
    int              sHover  = -1;
    float            sScroll = 0.f;

    int contentCount() const {
        if (mode == Mode::List) return (int)packs.size();
        return paramCounts.empty() ? 0 : paramCounts[0];
    }

    void dumpState() const {
        std::printf("  state: mode=%s row=%d (count=%d) buttonFocus=%d pointer=(%.1f,%.1f)\n",
                    modeName(mode), row, contentCount(), buttonFocus, mx, my);
    }

    void click(float x, float y) {
        mx = x;
        my = y;
        int b = layout.hitButton(x, y);
        int r = layout.hitRow(x, y);
        if (b >= 0) {
            std::printf("click(%.1f,%.1f) -> BUTTON %d\n", x, y, b);
            buttonFocus = b;
        } else if (r >= 0) {
            std::printf("click(%.1f,%.1f) -> ROW %d\n", x, y, r);
            row = r;
        } else {
            std::printf("click(%.1f,%.1f) -> (nothing)\n", x, y);
        }
        dumpState();
    }

    void key(int code) {
        switch (code) {
        case 38: // Up
            if (buttonFocus >= 0) buttonFocus = (buttonFocus >= 2) ? std::max(buttonFocus - 2, 0) : -1;
            else if (row > 0) --row;
            break;
        case 40: // Down
            if (buttonFocus >= 0) { if (buttonFocus < 2) buttonFocus = std::min(buttonFocus + 2, 4); }
            else if (row + 1 < contentCount()) ++row;
            else buttonFocus = 2;
            break;
        case 37: // Left
            if (buttonFocus >= 2) buttonFocus = std::max(buttonFocus - 1, 2);
            else if (buttonFocus >= 0) buttonFocus = 0;
            else if (row > 0) --row;
            break;
        case 39: // Right
            if (buttonFocus >= 2) buttonFocus = std::min(buttonFocus + 1, 4);
            else if (buttonFocus >= 0) buttonFocus = 1;
            else if (row + 1 < contentCount()) ++row;
            break;
        case 13: // Enter
            if (buttonFocus >= 0) std::printf("key 13 -> ACTIVATE BUTTON %d\n", buttonFocus);
            else std::printf("key 13 -> ACTIVATE ROW %d\n", row);
            break;
        case 27: // Esc
            std::printf("key 27 -> CLOSE\n");
            break;
        default:
            std::printf("key %d -> (ignored)\n", code);
            break;
        }
        dumpState();
    }

    // ---------------------------------------------------------------- 设置页

    SettingPageStyle sStyle() const { return sPage < 0 ? SettingPageStyle::ButtonGrid : SettingPageStyle::Rows; }

    void sRebuild() {
        sLayout = buildSettingsLayout(width, height, settings, sView, sStyle(), sHover, sScroll);
        sScroll = sLayout.scroll;
    }

    // 顶层分类页。
    void sEnterTop() {
        sPage   = -1;
        sView   = settingsTopLevelView(settings);
        sIndex  = sView.empty() ? -1 : sView.front();
        sHover  = -1;
        sScroll = 0.f;
        sRebuild();
    }

    // 打开某个分类（下一层）。
    void sOpenCategory(int categoryIndex) {
        if (categoryIndex < 0 || (std::size_t)categoryIndex >= settings.items.size()) return;
        sPage   = categoryIndex;
        sView   = settingsCategoryView(settings, categoryIndex);
        sIndex  = settingsFirstSelectable(settings, sView);
        sHover  = -1;
        sScroll = 0.f;
        sRebuild();
        std::printf("open category '%s' (%zu item(s))\n",
                    settings.items[(std::size_t)categoryIndex].label.c_str(), sView.size());
    }

    // 「返回」：先回上一层，顶层就什么都不做（游戏里会退回光影包列表）。
    void sBack() {
        if (sPage < 0) {
            std::printf("back -> (already at top level; the game would return to the pack list)\n");
            return;
        }
        int const previous = sPage;
        sPage   = -1;
        sView   = settingsTopLevelView(settings);
        sIndex  = previous;
        sHover  = -1;
        sScroll = 0.f;
        sRebuild();
        std::printf("back -> top level (selected category %d)\n", previous);
    }

    // 把指针放到某处：按当前版式算出悬停项（滑块靠它展开）。
    void sHoverAt(float x, float y) {
        mx = x;
        my = y;
        sHover = settingsHitTest(sLayout, x, y);
        sRebuild();
        std::printf("shover(%.1f,%.1f) -> item %d\n", x, y, sHover);
    }

    SettingBox const* sBoxFor(int index) const {
        for (auto const& b : sLayout.slots) {
            if (b.index == index) return &b;
        }
        return nullptr;
    }

    void sEnsureVisible() {
        if (auto const* box = sBoxFor(sIndex)) {
            if (box->row_.y0 < sLayout.viewport.y0) sScroll -= (sLayout.viewport.y0 - box->row_.y0);
            else if (box->row_.y1 > sLayout.viewport.y1) sScroll += (box->row_.y1 - sLayout.viewport.y1);
        }
        sScroll = std::max(0.f, std::min(sScroll, std::max(sLayout.maxScroll, 0.f)));
    }

    void sMove(int dx, int dy) {
        int const next = settingsMove(sLayout, sIndex, dx, dy);
        if (next >= 0 && next != sIndex) {
            sIndex = next;
            sEnsureVisible();
            return;
        }
        if (dx != 0 && sIndex >= 0) settingsAdjust(settings, sIndex, dx); // 到头了就微调值
    }

    void sDumpOverview() const {
        std::printf("  settings: page=%s view=%zu item(s), %zu visible, contentHeight=%.1f maxScroll=%.1f scroll=%.1f\n",
                    sPage < 0 ? "top(categories)" : settings.items[(std::size_t)sPage].label.c_str(),
                    sView.size(), sLayout.visible.size(), sLayout.contentHeight, sLayout.maxScroll, sLayout.scroll);
    }

    void sDumpSelected() const {
        if (sIndex < 0 || (std::size_t)sIndex >= settings.items.size()) {
            std::printf("  settings selected: (none)\n");
            return;
        }
        auto const& item = settings.items[(std::size_t)sIndex];
        if (item.kind == SettingKind::Category) {
            std::printf("  settings selected: #%d [category] %s\n", sIndex, item.label.c_str());
            return;
        }
        std::printf("  settings selected: #%d %s = %s%s\n", sIndex, item.key.c_str(), item.value.c_str(),
                    settingsIsOn(item) && item.kind == SettingKind::Bool ? " (on)" : "");
    }

    void sClick(float x, float y) {
        mx = x;
        my = y;
        // 游戏里的顺序是"渲染（带悬停）→ 点击"：先按当前指针更新一次悬停并重建版式，
        // 再拿这份版式做命中。滑块因此会在按下前就已经展开，点到哪都能拖。
        sHover = settingsHitTest(sLayout, x, y);
        sRebuild();
        SettingPart part = SettingPart::None;
        int const   idx  = settingsHitTest(sLayout, x, y, &part);
        if (idx < 0) {
            if (sLayout.scrollTrack.contains(x, y) && sLayout.maxScroll > 0.5f) {
                float const trackH = sLayout.scrollTrack.y1 - sLayout.scrollTrack.y0;
                float const t      = std::max(0.f, std::min((y - sLayout.scrollTrack.y0) / std::max(trackH, 1.f), 1.f));
                sScroll            = t * sLayout.maxScroll;
                sRebuild();
                std::printf("sclick(%.1f,%.1f) -> SCROLLBAR t=%.2f scroll=%.1f\n", x, y, t, sScroll);
                sDumpOverview();
                return;
            }
            std::printf("sclick(%.1f,%.1f) -> (nothing)\n", x, y);
            return;
        }
        sIndex      = idx;
        sHover      = idx; // 鼠标停在这项上（滑块会展开）
        auto const* box = sBoxFor(idx);
        std::printf("sclick(%.1f,%.1f) -> ITEM %d [%s] part=%s\n", x, y, idx,
                    settings.items[(std::size_t)idx].key.c_str(),
                    part == SettingPart::Control ? "control" : "label");
        // 顶层的分类按钮：点开进入下一层。
        if (settings.items[(std::size_t)idx].kind == SettingKind::Category) {
            sOpenCategory(idx);
            sDumpOverview();
            return;
        }
        if (part == SettingPart::Control) {
            auto const& item = settings.items[(std::size_t)idx];
            if (item.kind == SettingKind::Slider && box) {
                float const t = std::max(
                    0.f, std::min((x - box->control.x0) / std::max(box->control.x1 - box->control.x0, 1.f), 1.f));
                settingsSetSlider(settings, idx, t);
                std::printf("              slider drag t=%.2f\n", t);
            } else {
                settingsAdjust(settings, idx, 1);
            }
        }
        sEnsureVisible();
        sRebuild();
        sDumpSelected();
    }

    void sKey(int code) {
        switch (code) {
        case 38: sMove(0, -1); break;
        case 40: sMove(0, 1);  break;
        case 37: sMove(-1, 0); break;
        case 39: sMove(1, 0);  break;
        case 13: // Enter：分类按钮=打开下一层；参数=改值
            if (sIndex >= 0 && (std::size_t)sIndex < settings.items.size() &&
                settings.items[(std::size_t)sIndex].kind == SettingKind::Category) {
                sOpenCategory(sIndex);
                sDumpOverview();
                return;
            }
            if (sIndex >= 0) settingsAdjust(settings, sIndex, 1);
            break;
        case 219: if (sIndex >= 0) settingsAdjust(settings, sIndex, -1); break;
        case 221: if (sIndex >= 0) settingsAdjust(settings, sIndex, 1); break;
        default: std::printf("skey %d -> (ignored)\n", code); return;
        }
        sRebuild();
        std::printf("skey %d ->\n", code);
        sDumpSelected();
    }

    void sGrid() const {
        std::printf("  settings slots (all %zu):\n", sLayout.slots.size());
        for (auto const& b : sLayout.slots) {
            bool const vis = sLayout.viewport.y0 <= b.row_.y0 && b.row_.y1 <= sLayout.viewport.y1;
            std::printf("   idx=%-2d row=%-2d col=%-2d exp=%d %s row[%.0f,%.0f x %.0f,%.0f]\n", b.index, b.row,
                        b.column, (int)b.expanded, vis ? "vis" : "   ", b.row_.x0, b.row_.x1, b.row_.y0, b.row_.y1);
        }
    }

    void ascii() const {
        std::printf("  rows: %d, buttons at y=[%.0f..%.0f] and [%.0f..%.0f]\n", layout.rowCount, layout.buttons[0].y0,
                    layout.buttons[0].y1, layout.buttons[2].y0, layout.buttons[2].y1);
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string packsDir = argc > 1 ? argv[1] : "data/shaderpacks";
    float       width    = argc > 2 ? (float)std::atof(argv[2]) : 640.f;
    float       height   = argc > 3 ? (float)std::atof(argv[3]) : 371.f;

    auto& store = ShaderPackStore::getInstance();
    // 用包目录下的临时 generated，避免污染真实目录。
    store.scan(packsDir, packsDir + "/.sim_generated");

    Sim sim;
    sim.width  = width;
    sim.height = height;
    sim.layout = buildLayout(width, height);
    for (auto const& p : store.packs()) {
        sim.packs.push_back(p.name);
        sim.paramCounts.push_back((int)p.parameters.size());
        std::printf("pack '%s'  %zu parameter(s)\n", p.name.c_str(), p.parameters.size());
    }
    if (sim.packs.empty()) std::printf("(no packs found in %s)\n", packsDir.c_str());

    std::printf("screen %.0fx%.0f  rows=%d buttons=%d\n", width, height, sim.layout.rowCount,
                sim.layout.buttonCount);
    for (int i = 0; i < sim.layout.buttonCount; ++i) {
        auto const& r = sim.layout.buttons[i];
        std::printf("  B%d x[%.0f,%.0f] y[%.0f,%.0f] center=(%.0f,%.0f)\n", i, r.x0, r.x1, r.y0, r.y1,
                    (r.x0 + r.x1) / 2.f, (r.y0 + r.y1) / 2.f);
    }
    sim.dumpState();

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string        cmd;
        in >> cmd;
        if (cmd.empty()) continue;
        if (cmd == "q") break;
        if (cmd == "click") {
            float x = 0, y = 0;
            in >> x >> y;
            sim.click(x, y);
        } else if (cmd == "move") {
            in >> sim.mx >> sim.my;
            std::printf("move -> (%.1f,%.1f) hitButton=%d hitRow=%d\n", sim.mx, sim.my,
                        sim.layout.hitButton(sim.mx, sim.my), sim.layout.hitRow(sim.mx, sim.my));
        } else if (cmd == "key") {
            int code = 0;
            in >> code;
            sim.key(code);
        } else if (cmd == "grid") {
            sim.ascii();
        } else if (cmd == "list") {
            sim.mode = Mode::List;
            sim.row = 0;
            sim.buttonFocus = -1;
            sim.dumpState();
        } else if (cmd == "1") {
            sim.mode = Mode::List;
            sim.row = 0;
            sim.buttonFocus = -1;
            sim.dumpState();
        } else if (cmd == "2" || cmd == "settings") {
            sim.mode = Mode::Settings;
            sim.sEnterTop();
            std::printf("enter settings page (top level)\n");
            sim.sDumpOverview();
            sim.sDumpSelected();
        } else if (cmd == "scat") {
            int index = -1;
            in >> index;
            if (index >= 0 && index < (int)sim.sView.size()) index = sim.sView[(std::size_t)index];
            sim.sOpenCategory(index);
            sim.sDumpOverview();
        } else if (cmd == "sback") {
            sim.sBack();
            sim.sDumpOverview();
        } else if (cmd == "shover") {
            float x = 0, y = 0;
            in >> x >> y;
            sim.sHoverAt(x, y);
        } else if (cmd == "swheel") {
            float ticks = 0;
            in >> ticks;
            // 正值 = 向下滚（看后面的内容）。
            sim.sScroll = std::max(0.f, std::min(sim.sScroll + ticks * 26.f, sim.sLayout.maxScroll));
            sim.sRebuild();
            sim.sDumpOverview();
        } else if (cmd == "sgrid") {
            sim.sGrid();
        } else if (cmd == "sc") {
            float x = 0, y = 0;
            in >> x >> y;
            sim.sClick(x, y);
        } else if (cmd == "sk") {
            int code = 0;
            in >> code;
            sim.sKey(code);
        } else if (cmd == "ssel") {
            sim.sDumpSelected();
        } else {
            std::printf("unknown: %s\n", cmd.c_str());
        }
    }
    return 0;
}
