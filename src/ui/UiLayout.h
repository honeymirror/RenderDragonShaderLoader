#pragma once

#include <string>

// 界面几何与命中测试。刻意不依赖任何游戏类型，方便脱离游戏跑测试或导出 SVG 检查版式。
namespace renderdragon_shader_loader::ui {

// 列表页最多能排多少个行槽（第 0 行是"光影：已启用/已禁用"总开关，其余是包）。
// 取够大的值即可：实际行数由窗口高度决定，这个上限只是给数组定个界。
constexpr int kMaxListRows = 40;

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] bool valid() const { return x1 > x0 && y1 > y0; }
    [[nodiscard]] bool contains(float x, float y) const {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
};

struct Layout {
    Rect  rows[kMaxListRows];
    int   rowCount = 0;
    Rect  buttons[8];
    int   buttonCount = 0;
    // 右下角“透明预览”按钮（只有图标，不受底栏布局影响）。
    Rect  previewButton;
    float contentTop   = 0;
    float rowHeight    = 0;
    float centerX      = 0;
    float contentWidth = 0;
    // “+ 拖放光影包至此以添加”那一行的顶端：固定贴在中间栏最底部（不再飘在最后一个包下面）。
    float hintTop      = 0;

    // 命中测试：返回下标，-1 表示没命中。
    [[nodiscard]] int hitButton(float x, float y) const;
    [[nodiscard]] int hitRow(float x, float y) const;
};

Layout buildLayout(float width, float height);

// 顶栏下方那条分界线的 y（顶栏高度）。
constexpr float chromeHeaderLineY = 40.f;
// 底栏（按钮区）顶部分界线的 y。列表页与设置页共用同一套 chrome 尺寸。
float chromeBottomBarTop(float height);

// 把布局画成 SVG（调试用，浏览器/VS Code 预览直接看版式）。
std::string layoutToSvg(Layout const& layout, float width, float height);

} // namespace renderdragon_shader_loader::ui
