#include "ui/UiLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace renderdragon_shader_loader::ui {
namespace {

constexpr float rowHeight   = 16.f;
constexpr int   maxRowCount = kMaxListRows;

} // namespace

// 底栏第一排按钮的顶边 - 8：列表页与设置页共用，保证两条分界线对齐。
float chromeBottomBarTop(float height) {
    float const bottomRow2Y1 = height - 10.f;
    float const bottomRow2Y0 = bottomRow2Y1 - 20.f;
    float const bottomRow1Y1 = bottomRow2Y0 - 2.f;
    float const bottomRow1Y0 = bottomRow1Y1 - 20.f;
    return bottomRow1Y0 - 8.f;
}

float contentColumnWidth(float width, float preferredMax) {
    float const capForCorners = width - cornerButtonReserve;
    float       w             = std::min({width - 40.f, preferredMax, capForCorners});
    // 屏幕窄到连角落按钮都摆不下时，宁可让内容列很窄，也不要把它们叠在一起。
    if (w < 24.f) w = std::max(capForCorners, 24.f);
    return w;
}

int Layout::hitButton(float x, float y) const {
    for (int i = 0; i < buttonCount; ++i) {
        if (buttons[i].contains(x, y)) return i;
    }
    return -1;
}

int Layout::hitRow(float x, float y) const {
    for (int i = 0; i < rowCount; ++i) {
        if (rows[i].contains(x, y)) return i;
    }
    return -1;
}

Layout buildLayout(float width, float height) {
    Layout L;
    if (width <= 0.f || height <= 0.f) return L;

    L.centerX      = std::floor(width / 2.f);
    // 内容列要给左右两个角落按钮（左「设置」/ 右「全透明」）留出位置：
    // 以前这里只按 `width - 40` 取宽度、窄屏还会回退到 `width - 20`，
    // 结果标度 100%（GUI 宽度小）时底栏最右的按钮直接压在右下角按钮上。
    L.contentWidth = contentColumnWidth(width, 420.f);

    float const left  = L.centerX - L.contentWidth / 2.f;
    float const right = L.centerX + L.contentWidth / 2.f;

    // 下排贴底，上排紧贴其上。
    float bottomRow2Y1 = height - 10.f;
    float bottomRow2Y0 = bottomRow2Y1 - 20.f;
    float bottomRow1Y1 = bottomRow2Y0 - 2.f;
    float bottomRow1Y0 = bottomRow1Y1 - 20.f;

    float threW = (L.contentWidth - 12.f) / 3.f;

    L.buttons[0] = {left, bottomRow1Y0, L.centerX - 3.f, bottomRow1Y1};
    L.buttons[1] = {L.centerX + 3.f, bottomRow1Y0, right, bottomRow1Y1};
    L.buttons[2] = {left, bottomRow2Y0, left + threW, bottomRow2Y1};
    L.buttons[3] = {L.centerX - threW / 2.f, bottomRow2Y0, L.centerX + threW / 2.f, bottomRow2Y1};
    L.buttons[4] = {right - threW, bottomRow2Y0, right, bottomRow2Y1};
    L.buttonCount = 5;

    // Iris 风格：右下角独立的小图标按钮。
    constexpr float previewSize = 26.f;
    float const     bottomBarTop    = bottomRow1Y0 - 8.f;
    float const     bottomBarCenterY = (bottomBarTop + height) / 2.f;
    L.previewButton = {
        width - previewSize - 10.f,
        bottomBarCenterY - previewSize / 2.f,
        width - 10.f,
        bottomBarCenterY + previewSize / 2.f,
    };
    // 左下角同尺寸的「设置」按钮（镜像 previewButton）：底栏左侧的模组设置入口。
    L.settingsButton = {
        10.f,
        bottomBarCenterY - previewSize / 2.f,
        10.f + previewSize,
        bottomBarCenterY + previewSize / 2.f,
    };
    L.compatibilityButton = {10.f, 7.f, 36.f, 33.f};

    L.contentTop = 46.f;
    L.rowHeight  = rowHeight;

    // 中间栏从内容顶端一直排到按钮区上方那条分界线；**最底下一行留给拖放提示**，
    // 这样列表能吃满整栏（之前固定 12 行，屏幕再高也只能显示 11 个包）。
    float const contentBottom = bottomRow1Y0 - 8.f;
    float const usable        = std::max(contentBottom - L.contentTop - rowHeight, rowHeight);
    int const   maxRows       = (int)std::floor(usable / rowHeight);
    L.rowCount                = std::clamp(maxRows, 1, maxRowCount);

    for (int i = 0; i < L.rowCount; ++i) {
        float y0    = L.contentTop + (float)i * rowHeight;
        L.rows[i]   = {left, y0, right, y0 + rowHeight - 2.f};
    }
    L.hintTop = L.contentTop + (float)L.rowCount * rowHeight;
    return L;
}

std::string layoutToSvg(Layout const& L, float width, float height) {
    std::string svg;
    char        buf[512];

    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"";
    std::snprintf(buf, sizeof(buf), "%g\" height=\"%g\" viewBox=\"0 0 %g %g\">\n", width, height, width, height);
    svg += buf;
    svg += "<rect width=\"100%\" height=\"100%\" fill=\"#101014\"/>\n";

    // 顶栏 / 底栏各铺一层很淡的底（比中间更透明），中间内容区更实一点（和 UiScreen 一致）。
    if (L.buttonCount > 0) {
        float const top    = 40.f;
        float const bottom = L.buttons[0].y0 - 8.f;
        if (bottom > top) {
            std::snprintf(
                buf,
                sizeof(buf),
                "<rect x=\"0\" y=\"0\" width=\"%g\" height=\"40\" fill=\"#050608\" fill-opacity=\"0.16\"/>\n"
                "<rect x=\"0\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"#050608\" fill-opacity=\"0.16\"/>\n"
                "<rect x=\"0\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"#050608\" fill-opacity=\"0.30\"/>\n"
                "<line x1=\"0\" y1=\"40\" x2=\"%g\" y2=\"40\" stroke=\"#9e9e9a\" stroke-width=\"1\"/>\n",
                width,
                bottom,
                width,
                height - bottom,
                top,
                width,
                bottom - top,
                width
            );
            svg += buf;
        }
    }
    std::snprintf(buf, sizeof(buf), "<text x=\"%g\" y=\"15\" fill=\"#fff\" font-size=\"12\" text-anchor=\"middle\">Shader Packs</text>\n", L.centerX);
    svg += buf;
    std::snprintf(buf, sizeof(buf), "<text x=\"%g\" y=\"29\" fill=\"#ddd\" font-size=\"12\" text-anchor=\"middle\">Select</text>\n", L.centerX);
    svg += buf;

    // 行
    for (int i = 0; i < L.rowCount; ++i) {
        auto const& r = L.rows[i];
        std::snprintf(
            buf,
            sizeof(buf),
            "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"none\" stroke=\"#4a90d9\" stroke-width=\"1\"/>\n",
            r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0
        );
        svg += buf;
        std::snprintf(buf, sizeof(buf), "<text x=\"%g\" y=\"%g\" fill=\"#4a90d9\" font-size=\"9\">R%d</text>\n", r.x0 + 2.f, r.y0 + 9.f, i);
        svg += buf;
    }

    // 分隔线（横穿整屏）
    if (L.buttonCount > 0) {
        std::snprintf(
            buf,
            sizeof(buf),
            "<line x1=\"0\" y1=\"%g\" x2=\"%g\" y2=\"%g\" stroke=\"#9e9e9a\" stroke-width=\"1\"/>\n",
            L.buttons[0].y0 - 8.f, width, L.buttons[0].y0 - 8.f
        );
        svg += buf;
    }

    // 按钮
    char const* names[8] = {"folder", "settings/list", "cancel", "apply", "done", "?", "?", "?"};
    for (int i = 0; i < L.buttonCount; ++i) {
        auto const& r = L.buttons[i];
        std::snprintf(
            buf,
            sizeof(buf),
            "<rect x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\" fill=\"#1a1a1e\" stroke=\"#5ac24a\" stroke-width=\"1\"/>\n",
            r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0
        );
        svg += buf;
        std::snprintf(
            buf,
            sizeof(buf),
            "<text x=\"%g\" y=\"%g\" fill=\"#5ac24a\" font-size=\"9\" text-anchor=\"middle\">B%d %s</text>\n",
            (r.x0 + r.x1) / 2.f,
            (r.y0 + r.y1) / 2.f + 3.f,
            i,
            names[i]
        );
        svg += buf;
    }

    svg += "</svg>\n";
    return svg;
}

} // namespace renderdragon_shader_loader::ui
