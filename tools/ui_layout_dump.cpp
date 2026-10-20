// 离线调试界面版式与命中测试（不需要启动游戏）。
//
// 编译 & 运行（在 RenderDragonShaderLoader/ 目录下）：
//   clang-cl /nologo /std:c++20 /EHsc /utf-8 /I src tools/ui_layout_dump.cpp ^
//     src/ui/UiLayout.cpp src/ui/UiSettings.cpp /Fe:build\ui_layout_dump.exe
//   build\ui_layout_dump.exe [width] [height] [out.svg]
//
// 检查列表页 + 设置页两层（顶层分类按钮网格 / 分类内的参数页），并导出 SVG。
// 断言失败会打印 FAIL 并返回非 0。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "ui/UiLayout.h"
#include "ui/UiSettings.h"

using namespace renderdragon_shader_loader::ui;

namespace {

int failures = 0;

void check(bool ok, std::string const& what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what.c_str());
        ++failures;
    }
}

float centerX(Rect const& r) { return (r.x0 + r.x1) / 2.f; }
float centerY(Rect const& r) { return (r.y0 + r.y1) / 2.f; }

bool overlaps(Rect const& a, Rect const& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

void dumpListLayout(Layout const& L, float width, float height, std::string const& svgPath) {
    std::printf("screen %.0fx%.0f  centerX=%.1f contentWidth=%.1f rows=%d buttons=%d\n",
                width, height, L.centerX, L.contentWidth, L.rowCount, L.buttonCount);

    std::printf("\n-- list buttons --\n");
    for (int i = 0; i < L.buttonCount; ++i) {
        auto const& r = L.buttons[i];
        std::printf("B%d  x[%7.1f,%7.1f] y[%7.1f,%7.1f]\n", i, r.x0, r.x1, r.y0, r.y1);
    }
    for (int i = 0; i < L.buttonCount; ++i) {
        auto const& r   = L.buttons[i];
        int         hit = L.hitButton(centerX(r), centerY(r));
        check(hit == i, "list button " + std::to_string(i) + " center hit " + std::to_string(hit));
    }
    for (int i = 0; i < L.rowCount; ++i) {
        auto const& r   = L.rows[i];
        int         hit = L.hitRow(centerX(r), centerY(r));
        check(hit == i, "list row " + std::to_string(i) + " center hit " + std::to_string(hit));
    }
    check(L.hitButton(2.f, 2.f) == -1, "top-left is not a button");
    check(L.hitRow(2.f, height / 2.f) == -1, "far left is not a row");
    for (int i = 0; i < L.buttonCount; ++i) {
        for (int j = i + 1; j < L.buttonCount; ++j) {
            check(!overlaps(L.buttons[i], L.buttons[j]),
                  "list buttons " + std::to_string(i) + "/" + std::to_string(j) + " overlap");
        }
    }

    std::ofstream out(svgPath);
    if (out) out << layoutToSvg(L, width, height);
}

// 设置页的 SVG：把每个 slot 的行 / 控件 / 名字框画出来，方便肉眼检查版式。
void writeSettingsSvg(SettingsLayout const& S, SettingsModel const& model, float width, float height,
                      std::string const& path) {
    (void)model;
    std::ofstream out(path);
    if (!out) return;
    char buf[512];
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#101014\"/>\n";
    std::snprintf(buf, sizeof(buf),
                  "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"#1a1a1e\" stroke=\"#666\"/>\n",
                  S.viewport.x0, S.viewport.y0, S.viewport.x1 - S.viewport.x0, S.viewport.y1 - S.viewport.y0);
    out << buf;
    for (auto const& b : S.slots) {
        bool const visible = b.row_.y1 >= S.viewport.y0 && b.row_.y0 <= S.viewport.y1;
        if (!visible) continue;
        std::snprintf(buf, sizeof(buf),
                      "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"%s\" stroke=\"#888\"/>\n",
                      b.row_.x0, b.row_.y0, b.row_.x1 - b.row_.x0, b.row_.y1 - b.row_.y0,
                      b.expanded ? "#264" : (b.button ? "#334" : "none"));
        out << buf;
        if (b.control.valid()) {
            std::snprintf(buf, sizeof(buf),
                          "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"none\" stroke=\"#4a90d9\"/>\n",
                          b.control.x0, b.control.y0, b.control.x1 - b.control.x0, b.control.y1 - b.control.y0);
            out << buf;
        }
        // 展开的滑块不再有"行内的名字框"：名字改由鼠标旁的浮层显示（UiScreen 里画），
        // 所以这里只画整格轨道。
    }
    for (int i = 0; i < S.headerCount; ++i) {
        std::snprintf(buf, sizeof(buf),
                      "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"none\" stroke=\"#5ac24a\"/>\n",
                      S.header[i].x0, S.header[i].y0, S.header[i].x1 - S.header[i].x0,
                      S.header[i].y1 - S.header[i].y0);
        out << buf;
    }
    std::snprintf(buf, sizeof(buf),
                  "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"none\" stroke=\"#5ac24a\"/>\n",
                  S.headerBack.x0, S.headerBack.y0, S.headerBack.x1 - S.headerBack.x0,
                  S.headerBack.y1 - S.headerBack.y0);
    out << buf;
    out << "</svg>\n";
}

void checkHeaderButtons(SettingsLayout const& S) {
    for (int i = 0; i < S.headerCount; ++i) {
        check(S.header[i].valid(), "settings header button " + std::to_string(i) + " valid");
        check(!overlaps(S.header[i], S.headerBack), "settings back button does not overlap header buttons");
        for (int j = i + 1; j < S.headerCount; ++j) {
            check(!overlaps(S.header[i], S.header[j]), "settings header buttons do not overlap");
        }
    }
}

// 顶层：分类按钮（两列网格）。点开进入下一层，返回回上一层。
std::vector<int> verifySettingsTopLevel(SettingsModel const& model, float width, float height,
                                        std::string const& svgPath) {
    std::vector<int> const view = settingsTopLevelView(model);
    check((int)view.size() >= 4, "demo data has at least 4 categories");

    SettingsLayout const S =
        buildSettingsLayout(width, height, model, view, SettingPageStyle::ButtonGrid, -1, 0.f);
    std::printf("\n-- settings top level: %d categories, %d visible, maxScroll=%.1f --\n", (int)view.size(),
                (int)S.visible.size(), S.maxScroll);

    check((int)S.slots.size() == (int)view.size(), "top page shows every category as a button");
    check(!S.visible.empty(), "top page has visible buttons");
    float const halfW = (S.content.x1 - S.content.x0 - 4.f) / 2.f;
    for (int slotIndex : S.visible) {
        auto const& b = S.slots[(std::size_t)slotIndex];
        check(b.button, "top-level entry is drawn as a button");
        check(!b.control.valid(), "top-level button has no inner value box");
        check(std::abs((b.row_.x1 - b.row_.x0) - halfW) < 1.f, "top-level button is half the content width");
        SettingPart part = SettingPart::None;
        int const   hit  = settingsHitTest(S, centerX(b.row_), centerY(b.row_), &part);
        check(hit == b.index, "top-level button center hits itself");
    }
    checkHeaderButtons(S);
    SettingsLayout const clamped =
        buildSettingsLayout(width, height, model, view, SettingPageStyle::ButtonGrid, -1, 1e6f);
    check(std::abs(clamped.scroll - clamped.maxScroll) < 0.01f, "top-level scroll clamps to maxScroll");

    writeSettingsSvg(S, model, width, height, svgPath);
    return view;
}

// 分类页：参数（两列 / 整行），滑块悬停即展开、移开复原。
void verifySettingsCategoryPage(SettingsModel const& model, std::vector<int> const& topView, float width,
                                float height, std::string const& svgPath) {
    // 找内容最多的那个分类（一定含整行滑块）：既能验证悬停展开，也能验证滚动。
    int category = -1;
    std::size_t bestCount = 0;
    for (int index : topView) {
        auto const items = settingsCategoryView(model, index);
        bool       hasFullSlider = false;
        for (int item : items) {
            if (model.items[(std::size_t)item].kind == SettingKind::Slider &&
                model.items[(std::size_t)item].span == SettingSpan::Full) {
                hasFullSlider = true;
                break;
            }
        }
        if (hasFullSlider && items.size() > bestCount) {
            bestCount = items.size();
            category  = index;
        }
    }
    check(category >= 0, "demo data has a category containing a full-width slider");
    if (category < 0) return;

    std::vector<int> const view = settingsCategoryView(model, category);
    check(!view.empty(), "category page has items");

    int hoverTarget = -1;
    for (int index : view) {
        if (model.items[(std::size_t)index].kind == SettingKind::Slider &&
            model.items[(std::size_t)index].span == SettingSpan::Full) {
            hoverTarget = index;
            break;
        }
    }

    // 鼠标不在任何项上：全部是"紧凑态"。
    SettingsLayout const idle =
        buildSettingsLayout(width, height, model, view, SettingPageStyle::Rows, -1, 0.f);
    // 鼠标悬停在滑块上：它展开占满整个文本框。
    SettingsLayout const S =
        buildSettingsLayout(width, height, model, view, SettingPageStyle::Rows, hoverTarget, 0.f);

    std::printf("\n-- settings category page: %d item(s), %d visible, contentHeight=%.1f maxScroll=%.1f --\n",
                (int)view.size(), (int)S.visible.size(), S.contentHeight, S.maxScroll);
    std::printf("   hovered slider idx=%d (expanded=%d)\n", hoverTarget,
                (int)(hoverTarget >= 0));

    auto boxFor = [&](SettingsLayout const& L, int index) -> SettingBox const* {
        for (auto const& b : L.slots) {
            if (b.index == index) return &b;
        }
        return nullptr;
    };

    check(!S.visible.empty(), "category page has visible rows");
    check(std::abs(S.maxScroll - std::max(0.f, S.contentHeight - (S.viewport.y1 - S.viewport.y0))) < 0.5f,
          "maxScroll matches content height minus viewport");
    // 内容超过一屏就必须能滚；没超过就不该有滚动条。
    if (S.contentHeight > (S.viewport.y1 - S.viewport.y0)) {
        check(S.maxScroll > 0.f, "settings content scrolls when it overflows the viewport");
    } else {
        check(S.maxScroll <= 0.01f, "settings content does not scroll when it fits");
    }

    // 未悬停时没有展开项；悬停整行滑块时它展开并占满整格。
    for (auto const& b : idle.slots) {
        check(!b.expanded, "no slider is expanded while nothing is hovered");
    }
    if (auto const* b = boxFor(S, hoverTarget)) {
        check(b->expanded, "hovered slider is expanded");
        check(std::abs(b->control.x0 - (b->row_.x0 + 1.f)) < 0.01f &&
                  std::abs(b->control.x1 - (b->row_.x1 - 1.f)) < 0.01f,
              "hovered slider fills its whole text box");
        // 名字不再占行内位置：展开态整格都是轨道，名字是鼠标旁的浮层（见 UiScreen::drawSettings）。
        check(std::abs(b->label.x0 - b->row_.x0) < 0.5f && std::abs(b->label.x1 - b->row_.x1) < 0.5f,
              "expanded slider has no in-row name box");
    }

    // 不悬停时，滑块是"名字在左、值框在右"的紧凑态。
    if (hoverTarget >= 0) {
        if (auto const* b = boxFor(idle, hoverTarget)) {
            check(!b->expanded, "unhovered slider is compact");
            check(b->control.valid() && (b->control.x1 - b->control.x0) < (b->row_.x1 - b->row_.x0) - 1.f,
                  "compact slider does not fill the whole row");
        }
    }

    // 两列并排：按版式同样的配对规则走，成对的半行项左右不重叠、合起来占满内容栏。
    int halfPairs = 0;
    for (int k = 0; k < (int)view.size();) {
        auto const& a = model.items[(std::size_t)view[(std::size_t)k]];
        if (a.kind == SettingKind::Category || a.span == SettingSpan::Full) {
            ++k;
            continue;
        }
        bool const paired = k + 1 < (int)view.size() &&
                            model.items[(std::size_t)view[(std::size_t)(k + 1)]].span == SettingSpan::Half &&
                            model.items[(std::size_t)view[(std::size_t)(k + 1)]].kind != SettingKind::Category;
        if (!paired) {
            ++k;
            continue;
        }
        auto const* ba = boxFor(S, view[(std::size_t)k]);
        auto const* bb = boxFor(S, view[(std::size_t)(k + 1)]);
        if (ba && bb) {
            check(ba->row == bb->row, "two-column items share a row");
            check(ba->column == 0 && bb->column == 1, "two-column items are left/right");
            check(!overlaps(ba->row_, bb->row_), "two-column items do not overlap");
            check(std::abs(ba->row_.x0 - S.content.x0) < 0.5f && std::abs(bb->row_.x1 - S.content.x1) < 0.5f,
                  "two-column row spans the content column");
        }
        ++halfPairs;
        k += 2;
    }
    check(halfPairs > 0, "demo data has two-column rows");

    // 整行：占满内容栏宽度。
    int fullRows = 0;
    for (int index : view) {
        auto const& it = model.items[(std::size_t)index];
        if (it.span != SettingSpan::Full) continue;
        auto const* b = boxFor(S, index);
        if (!b) continue;
        check(std::abs(b->row_.x0 - S.content.x0) < 0.5f && std::abs(b->row_.x1 - S.content.x1) < 0.5f,
              "full-width item spans the content column");
        ++fullRows;
    }
    check(fullRows > 0, "demo data has full-width rows");

    // 所有可视行都落在可视区内。
    for (int slotIndex : S.visible) {
        auto const& r = S.slots[(std::size_t)slotIndex].row_;
        check(r.y0 >= S.viewport.y0 - 0.5f && r.y1 <= S.viewport.y1 + 0.5f, "visible rows stay inside the viewport");
    }

    // 命中测试：可视项中心命中它自己；值框中心命中 Control。
    for (int slotIndex : S.visible) {
        auto const& b    = S.slots[(std::size_t)slotIndex];
        SettingPart part = SettingPart::None;
        int const   hit  = settingsHitTest(S, centerX(b.row_), centerY(b.row_), &part);
        check(hit == b.index, "settings row " + std::to_string(b.index) + " center hits itself");
        if (!b.control.valid()) continue;
        SettingPart cpart = SettingPart::None;
        int const   chit  = settingsHitTest(S, centerX(b.control), centerY(b.control), &cpart);
        check(chit == b.index && cpart == SettingPart::Control, "settings control hits as Control");
    }
    check(settingsHitTest(S, 2.f, height / 2.f) == -1, "far left is not a settings row");

    checkHeaderButtons(S);
    SettingsLayout const clampedEnd = buildSettingsLayout(width, height, model, view, SettingPageStyle::Rows,
                                                          hoverTarget, 1e6f);
    check(!clampedEnd.visible.empty(), "settings layout still draws rows when scrolled to the end");

    writeSettingsSvg(S, model, width, height, svgPath);
}

} // namespace

int main(int argc, char** argv) {
    float       width   = argc > 1 ? (float)std::atof(argv[1]) : 640.f;
    float       height  = argc > 2 ? (float)std::atof(argv[2]) : 371.f;
    std::string svgPath = argc > 3 ? argv[3] : "ui_layout.svg";

    Layout L = buildLayout(width, height);
    dumpListLayout(L, width, height, svgPath);

    auto derived = [](std::string const& base, std::string const& suffix) {
        auto dot = base.rfind(".svg");
        return (dot == std::string::npos) ? (base + suffix) : (base.substr(0, dot) + suffix + ".svg");
    };
    std::string const topPath = derived(svgPath, "-settings-top");
    std::string const catPath = derived(svgPath, "-settings-category");

    SettingsModel const model = makeDemoSettings();
    std::vector<int> const topView = verifySettingsTopLevel(model, width, height, topPath);
    verifySettingsCategoryPage(model, topView, width, height, catPath);
    // 再用一个很矮的窗口跑一遍：专门验证"内容超出可视区 ⇒ 可滚动 + 仍然画得出来"。
    verifySettingsCategoryPage(model, topView, width, 170.f, derived(svgPath, "-settings-scroll"));

    if (failures == 0) {
        std::printf("\nall assertions passed\n");
    } else {
        std::printf("\n%d assertion(s) FAILED\n", failures);
    }
    std::printf("SVG written to %s / %s / %s\n", svgPath.c_str(), topPath.c_str(), catPath.c_str());
    return failures == 0 ? 0 : 1;
}
