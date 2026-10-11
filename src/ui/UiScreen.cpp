#include "ui/UiScreen.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace renderdragon_shader_loader::ui {
namespace {

// 配色对齐 Iris：顶栏与按钮栏都全透明，只有中间内容区铺一层很淡的深色；
// 按钮是灰底灰框，鼠标悬停/键盘选中时换成白框 + 淡白填充。
constexpr Color panelColor{0.05f, 0.055f, 0.06f, 1.f}; // 中间内容区的底色
constexpr Color barColor{0.62f, 0.62f, 0.60f, 1.f};    // 分隔线
constexpr Color textColor{1.f, 1.f, 1.f, 1.f};
constexpr Color dimColor{0.72f, 0.72f, 0.70f, 1.f};
constexpr Color selectColor{1.f, 0.9f, 0.35f, 1.f}; // 选中文字（Iris 用黄色）
constexpr Color accentColor{0.42f, 0.765f, 0.286f, 1.f};
constexpr Color failColor{1.f, 0.5f, 0.4f, 1.f};
constexpr Color buttonFillActive{0.90f, 0.90f, 0.90f, 1.f}; // 悬停/选中：淡白填充
constexpr Color buttonBorder{0.78f, 0.78f, 0.78f, 1.f};     // 常态边框（灰）
constexpr Color buttonBorderActive{1.f, 1.f, 1.f, 1.f};     // 悬停/选中：白边框
// 设置页：数值文字用 Iris 那种蓝色（布尔值仍是白色），值框边框比按钮边框亮一点。
constexpr Color valueColor{0.36f, 0.36f, 1.f, 1.f};
constexpr Color innerBorderColor{0.86f, 0.86f, 0.84f, 1.f};

// 标题两行文字顶端。这个值是按游戏内实测校准的：游戏字体在 14 单位高的文本框里
// 只占上面约 10 单位，所以要在 0..40 的顶栏里视觉居中，起点得给到 8（线上下留白各约 9）。
constexpr float headerTextY  = 8.f;
constexpr float headerLineY  = 40.f;  // 顶栏下方那条横线的位置
constexpr float contentAlpha = 0.30f; // 中间内容区的不透明度（"稍微没那么透明"）
constexpr float chromeAlpha  = 0.16f; // 顶栏 / 底栏：比中间更透明的一层底

// 游戏字体在 14 单位高的文本框里只占上面约 10 单位，所以要让文字在框里"中部对齐"，
// 光靠 (h - 14) / 2 会偏高，得再往下压约 1.5 单位（数值按游戏内实测校准）。
constexpr float textCenterNudge = 1.5f;

// 一行文字在"槽位"里的顶端位置：槽位高 slotHeight，文字框 14，再压 textCenterNudge。
// 列表行槽位 16、按钮槽位 20，都走这一个公式，保证所有文字在同一套对齐规则上。
constexpr float textTopIn(float slotTop, float slotHeight) {
    return slotTop + (slotHeight - 14.f) / 2.f + textCenterNudge;
}

// 描一个 1px 矩形边框（按钮和列表行都用它，保证样式一致）。
void strokeRect(Canvas& canvas, Rect const& r, Color color, float alpha) {
    float const w = r.x1 - r.x0;
    float const h = r.y1 - r.y0;
    canvas.fill(r.x0, r.y0, w, 1.f, color, alpha);
    canvas.fill(r.x0, r.y1 - 1.f, w, 1.f, color, alpha);
    canvas.fill(r.x0, r.y0, 1.f, h, color, alpha);
    canvas.fill(r.x1 - 1.f, r.y0, 1.f, h, color, alpha);
}

// 滑标：Iris 用的是原版按钮贴图的一小块（`GuiUtil.drawButton(..., sliderPos, y + 4,
// ACTIVE_SLIDER_WIDTH, height - 8, ...)`）。我们没有那张贴图，就按用户要的样子画：
// **白边框 + 内芯用所在文本框的底色**（黑），看上去就是文本框里立着的一根小柱子。
void drawSliderThumb(Canvas& canvas, float x, float y, float w, float h, bool grabbed) {
    if (w <= 0.f || h <= 0.f) return;
    // 内芯 = 所在文本框的底色（和展开轨道的黑底同一档），抓住时再压一层很淡的白。
    canvas.fill(x, y, w, h, Color{0.f, 0.f, 0.f, 1.f}, 0.9f);
    if (grabbed) canvas.fill(x, y, w, h, Color{1.f, 1.f, 1.f, 1.f}, 0.18f);
    strokeRect(canvas, Rect{x, y, x + w, y + h}, Color{1.f, 1.f, 1.f, 1.f}, grabbed ? 1.f : 0.95f);
}

// 展开滑块的滑标几何：6 单位宽（Iris 的 ACTIVE_SLIDER_WIDTH），上下各留 2 —— 比原来长一些，
// 边距也更靠近所在文本框的上下边。
constexpr float sliderThumbW = 6.f;
constexpr float sliderThumbInsetY = 2.f;

Rect sliderThumbRect(Rect const& row, float fraction) {
    float const spanX0 = row.x0 + 4.f;
    float const spanX1 = row.x1 - 4.f - sliderThumbW;
    float const tx     = spanX0 + (spanX1 - spanX0) * std::clamp(fraction, 0.f, 1.f);
    return Rect{tx, row.y0 + sliderThumbInsetY, tx + sliderThumbW, row.y1 - sliderThumbInsetY};
}

// 指针 x → 滑块比例。和 sliderThumbRect 用同一套映射，保证"画出来的滑标"永远在指针下面。
float sliderFractionAt(Rect const& row, float x) {
    float const spanX0 = row.x0 + 4.f;
    float const spanX1 = row.x1 - 4.f - sliderThumbW;
    if (spanX1 <= spanX0) return 0.f;
    return std::clamp((x - spanX0) / (spanX1 - spanX0), 0.f, 1.f);
}

// 按住滚动条拖动：把"滑块顶端该在的位置"换算回滚动量。
// pointerY / grabDy = 指针 y 与按下时指针相对滑块顶端的偏移（保证拖起来不跳）。
float scrollFromHandle(float pointerY, float grabDy, Rect const& track, Rect const& handle, float maxScroll) {
    float const trackH  = track.y1 - track.y0;
    float const handleH = handle.y1 - handle.y0;
    float const span    = trackH - handleH;
    if (span <= 0.f || maxScroll <= 0.f) return 0.f;
    float const top = std::clamp(pointerY - grabDy, track.y0, track.y1 - handleH);
    return (top - track.y0) / span * maxScroll;
}

// 列表/参数行的高亮底：悬停 = 白框 + 淡白填充（Iris 那种鼠标反馈）；
// 选中 = 白框 + 黑底。没悬停也没选中就什么都不画（行本身是透明的）。
Rect rowBand(Rect const& r) { return Rect{r.x0 - 4.f, r.y0, r.x1 + 4.f, r.y1 + 2.f}; }

void drawRowHighlight(Canvas& canvas, Rect const& row, bool selected, bool hovered) {
    if (!selected && !hovered) return;
    Rect const  band = rowBand(row);
    float const w    = band.x1 - band.x0;
    float const h    = band.y1 - band.y0;
    if (selected) {
        canvas.fill(band.x0, band.y0, w, h, Color{0.f, 0.f, 0.f, 1.f}, 0.85f);
    } else {
        canvas.fill(band.x0, band.y0, w, h, Color{1.f, 1.f, 1.f, 1.f}, 0.35f);
    }
    strokeRect(canvas, band, Color{1.f, 1.f, 1.f, 1.f}, 0.95f);
}

constexpr float rowHeight = 16.f;

bool isOnValue(std::string const& v) {
    std::string s = v;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s == "on" || s == "true" || s == "yes" || s == "1";
}

// 数一个目录里有多少个 *.material.bin（只有文件系统依赖，UI 核心依旧不碰游戏类型）。
std::size_t countMaterialBins(std::filesystem::path const& dir) {
    std::error_code ec;
    std::size_t     count = 0;
    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename().string().ends_with(".material.bin")) ++count;
    }
    return count;
}

// 列表里显示的名字：zip 包显示**完整文件名**（Mirai.zip、test.zip），和 Iris 一致；
// 目录形式的包就显示目录名。内部用的 pack.name（去扩展名）保持不变——它决定
// generated/<名字>/ 和 <名字>.zip.txt 这些路径，不能跟着显示名改。
std::string packLabel(ShaderPack const& pack) {
    if (!pack.archivePath.empty()) return pack.archivePath.filename().string();
    return pack.name;
}

std::string formatNumber(double v, bool integer) {
    if (integer) return std::to_string((long long)std::llround(v));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

double parseNumber(std::string const& s, double fallback) {
    char*  end    = nullptr;
    double parsed = std::strtod(s.c_str(), &end);
    return (end != s.c_str() && *end == '\0') ? parsed : fallback;
}

// "#RRGGBB" -> Color；解析失败时退回中灰。
Color parseHexColor(std::string const& text) {
    if (text.size() == 7 && text[0] == '#') {
        char*  end = nullptr;
        long   v   = std::strtol(text.c_str() + 1, &end, 16);
        if (end == text.c_str() + 7) {
            return Color{
                (float)((v >> 16) & 0xFF) / 255.f,
                (float)((v >> 8) & 0xFF) / 255.f,
                (float)(v & 0xFF) / 255.f,
                1.f,
            };
        }
    }
    return Color{0.5f, 0.5f, 0.5f, 1.f};
}

// 把设置模型的值序列化成一行行 "key=value"，用来比较"有没有改过"。
std::string dumpSettings(SettingsModel const& model) {
    std::string out;
    for (auto const& item : model.items) {
        if (item.kind == SettingKind::Category) continue;
        out += item.key + "=" + item.value + "\n";
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------- 基础

std::string Screen::tr(std::string_view key) const {
    if (mHost.translate) {
        auto value = mHost.translate(key);
        if (!value.empty()) return value;
    }
    return std::string(key);
}

void Screen::log(std::string const& line) const {
    if (mHost.log) mHost.log(line);
}

// Iris 的 GuiUtil.playButtonClickSound()：所有按钮/参数操作都回一声原版点击音。
void Screen::click() const {
    if (mHost.playClick) mHost.playClick();
}

// 这个版本还没有编译链路（只加载光影包自带的 .material.bin），所以界面永远不会被"编译中"锁住。
// 保留这个开关：以后接上编译服务时，让它返回"是否正在编译"即可，所有输入路径都已经走它。
bool Screen::blocked() const { return false; }

void Screen::setPointer(float x, float y) {
    if (x >= 0.f && y >= 0.f) {
        mMouseX = x;
        mMouseY = y;
        // 游戏里鼠标移动不经过 MouseInputEvent（那边对 ActionMove 直接 return），
        // 每帧只靠渲染时 setPointer 喂当前位置。所以"按住左键拖滑块"必须在这里跟随，
        // 只写在 handleMouseMove 里的话，游戏内按住拖动会完全没反应。
        if (mMode == Mode::Settings && mSettingDragging && mMouseDown) updateSettingDrag(mMouseX, mMouseY);
        // 滚动条拖动同理，也得在这里跟：列表页和设置页共用一套。
        if ((mSettingScrollDragging || mListScrollDragging) && mMouseDown) updateScrollDrags(mMouseY);
    }
}

void Screen::setVisible(bool value) {
    if (value) {
        open();
    } else {
        // 面板被外部关掉（例如再按一次 K）：没应用的改动一律丢掉，等同「取消」。
        discardPending();
        close();
    }
}

void Screen::open() {
    // 每次打开面板都重扫一遍包目录：玩家刚丢进来的 *.zip 才会出现在列表里。
    refreshPacks();
    mNotice.clear();
    mNoticeFailed = false;

    mVisible     = true;
    mMode        = Mode::List;
    mRow         = mPack >= 0 ? mPack + 1 : 0; // 第 0 项是总开关
    mListFirst   = 0;                          // 列表从头开始滚
    mButtonFocus = -1;
    mListScrollDragging    = false;
    mSettingScrollDragging = false;
    mPending     = Pending::None;

    // 记下当前选项值：用来判断"参数有没有被改过"（只有「应用/完成」才会存盘/加载）。
    snapshotParameters();
}

void Screen::refreshPacks() {
    // 重扫会替换整个列表，所以先记住选中的包名，扫完按名字找回来。
    std::string keepName;
    {
        auto const& packs = ShaderPackStore::getInstance().packs();
        if (mPack >= 0 && (std::size_t)mPack < packs.size()) keepName = packs[(std::size_t)mPack].name;
    }
    if (mHost.rescanPacks) mHost.rescanPacks();
    {
        auto const& packs = ShaderPackStore::getInstance().packs();
        mPack             = -1;
        if (!keepName.empty()) {
            for (std::size_t i = 0; i < packs.size(); ++i) {
                if (packs[i].name == keepName) {
                    mPack = (int)i;
                    break;
                }
            }
        }
        if (mPack < 0 && !packs.empty()) mPack = 0;
    }
}

void Screen::close() {
    mVisible = false;
    mPending = Pending::None;
    if (mHost.closeScreen) mHost.closeScreen();
}

void Screen::dismiss() {
    // 游戏已经自行弹掉了占位屏幕；这里只回滚暂存状态，绝不在 onScreenExit
    // / 渲染回调里做重扫目录或解压这类文件系统工作。
    discardPending(false);
    mVisible = false;
    mPending = Pending::None;
}

// ---------------------------------------------------------------- 绘制

void Screen::drawHeader(Canvas& canvas, float width, float y, std::string const& title, std::string const& subtitle) {
    // 顶栏铺一层**比中间内容区更透明**的底，再画两行居中文字 + 底下一条贯穿整屏的分界线。
    canvas.fill(0.f, 0.f, width, headerLineY, panelColor, chromeAlpha);
    canvas.text(0.f, y, width, title, textColor, Align::Center, 1.f);
    canvas.text(0.f, y + 14.f, width, subtitle, dimColor, Align::Center, 1.f);
    canvas.fill(0.f, headerLineY, width, 1.f, barColor, 0.85f);
}

// 底栏（按钮区）：和顶栏同级的"更透明"底，从按钮上方那条分界线一直铺到屏幕底部。
void Screen::drawBottomBar(Canvas& canvas, float width, float height) {
    auto const& L = mLayout;
    if (L.buttonCount <= 0) return;
    float const top = L.buttons[0].y0 - 8.f;
    if (height <= top) return;
    canvas.fill(0.f, top, width, height - top, panelColor, chromeAlpha);
}

void Screen::notice(std::string text, bool failed) {
    mNotice       = std::move(text);
    mNoticeFailed = failed;
    // 同一条提示再触发一次也要重新计时（比如连点两次「重置」）。
    mStatusShown   = mNotice;
    mStatusShownAt = std::chrono::steady_clock::now();
    log("[ui] notice: " + mNotice);
}

// 状态行：显示界面自己的提示（notice），3 秒后淡出。
std::string Screen::effectiveStatus() const {
    return mNotice;
}

// 淡出曲线：0~3 秒不透明 → 3~4 秒线性淡出 → 4 秒后完全消失。
float Screen::statusAlpha(std::string const& message) const {
    if (message.empty()) return 0.f;
    if (message != mStatusShown) return 1.f; // 本帧刚出现，还没登记
    float const age = std::chrono::duration<float>(std::chrono::steady_clock::now() - mStatusShownAt).count();
    if (age <= 3.f) return 1.f;
    if (age >= 4.f) return 0.f;
    return 1.f - (age - 3.f);
}

// 文字变了就重新开始计时。
void Screen::trackStatus(std::string const& message) {
    if (message == mStatusShown) return;
    mStatusShown   = message;
    mStatusShownAt = std::chrono::steady_clock::now();
}

void Screen::drawStatusLine(Canvas& canvas, float width, float y) const {
    std::string const message = effectiveStatus();
    float const       alpha   = statusAlpha(message);
    if (message.empty() || alpha <= 0.02f) return;
    Color      color  = mNoticeFailed ? failColor : accentColor;
    color.a           = alpha;
    canvas.text(0.f, y, width, message, color, Align::Center, 1.f);
}

// 中间内容区：全宽一层很淡的深色，让列表/参数在亮背景上也看得清（比顶栏和按钮栏实一点）。
void Screen::drawContentBackdrop(Canvas& canvas, float width) {
    auto const& L = mLayout;
    // 上边贴住顶栏那条线，下边一直铺到按钮区上方那条分界线——中间不留透明缝。
    float const top    = headerLineY;
    float const bottom = (L.buttonCount > 0) ? (L.buttons[0].y0 - 8.f) : (L.contentTop + 100.f);
    if (bottom <= top) return;
    canvas.fill(0.f, top, width, bottom - top, panelColor, contentAlpha);
}

void Screen::drawButton(Canvas& canvas, Rect const& r, std::string const& value, int index) {
    if (!r.valid()) return;
    bool hovered = r.contains(mMouseX, mMouseY);
    bool focused = mButtonFocus == index; // 键盘焦点
    bool active  = hovered || focused;

    float width  = r.x1 - r.x0;
    float height = r.y1 - r.y0;

    // 常态：内部完全透明，只有一个灰框、白色文字；悬停/选中：边框变白，内部淡淡填一层白。
    // 文字**始终白色**（按需求：选中也不转黑），所以填充压得淡一点，保证白字还看得清。
    if (active) canvas.fill(r.x0, r.y0, width, height, buttonFillActive, 0.26f);
    strokeRect(canvas, r, active ? buttonBorderActive : buttonBorder, 0.9f);

    canvas.text(r.x0, textTopIn(r.y0, height), width, value, textColor, Align::Center, 1.f);
}

void Screen::drawPreviewButton(Canvas& canvas) {
    Rect const& r = mLayout.previewButton;
    if (!r.valid()) return;

    bool const hovered = r.contains(mMouseX, mMouseY);
    float const w      = r.x1 - r.x0;
    float const h      = r.y1 - r.y0;

    // 和底栏按钮同风格：常态内部透明、灰框白图标；悬停时白框 + 淡白填充。
    if (hovered) canvas.fill(r.x0, r.y0, w, h, buttonFillActive, 0.26f);
    strokeRect(canvas, r, hovered ? buttonBorderActive : buttonBorder, 0.9f);

    float const lineX0  = r.x0 + 6.f;
    float const lineX1  = r.x1 - 6.f;
    float const centerY = (r.y0 + r.y1) / 2.f;
    for (float dy : {-6.f, 0.f, 6.f}) {
        canvas.fill(lineX0, centerY + dy - 1.f, lineX1 - lineX0, 2.f, textColor, 1.f);
    }

    // 透明预览模式：像 Iris 那样在按钮上压一个红色斜叉，表示栏目已全透明。
    if (mTransparentPreview) {
        auto diagonal = [&](float x0, float y0, float x1, float y1) {
            int const steps = (int)std::max(std::abs(x1 - x0), std::abs(y1 - y0));
            for (int i = 0; i <= steps; ++i) {
                float const t = steps == 0 ? 0.f : (float)i / (float)steps;
                float const x = x0 + (x1 - x0) * t;
                float const y = y0 + (y1 - y0) * t;
                canvas.fill(x - 1.f, y - 1.f, 2.f, 2.f, Color{1.f, 0.15f, 0.12f, 1.f}, 1.f);
            }
        };
        diagonal(r.x0 + 4.f, r.y0 + 4.f, r.x1 - 4.f, r.y1 - 4.f);
        diagonal(r.x1 - 4.f, r.y0 + 4.f, r.x0 + 4.f, r.y1 - 4.f);
    }
}

// Iris 风格右侧滚动条：贴屏幕最右缘（"中间那栏的最右边"），暗色轨道 + 浅灰滑块。
void Screen::drawScrollBar(Canvas& canvas, Rect const& track, Rect const& handle) {
    if (!track.valid()) return;
    canvas.fill(track.x0, track.y0, track.x1 - track.x0, track.y1 - track.y0, Color{0.f, 0.f, 0.f, 1.f}, 0.35f);
    if (!handle.valid()) return;
    canvas.fill(handle.x0, handle.y0, handle.x1 - handle.x0, handle.y1 - handle.y0,
                Color{0.80f, 0.80f, 0.78f, 1.f}, 0.9f);
}

void Screen::drawList(Canvas& canvas, float width) {
    drawContentBackdrop(canvas, width);
    drawHeader(canvas, width, headerTextY, tr("Shader Packs"), tr("Select"));

    auto const& packs = ShaderPackStore::getInstance().packs();
    auto const& L     = mLayout;

    // 第 0 行：Iris 那个"光影：已启用 / 光影：已禁用"总开关（带边框的宽条目）。
    // 几何用和下面行高亮**完全一样**的 rowBand()，否则左右/上下会比包条目窄一圈，看起来不齐。
    {
        Rect const slot = L.rows[0];
        if (slot.valid()) {
            bool const sel    = mRow == 0;
            bool const hov    = slot.contains(mMouseX, mMouseY);
            bool const active = sel || hov;
            Rect const box    = rowBand(slot);
            if (active) canvas.fill(box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0, Color{1.f, 1.f, 1.f, 1.f}, 0.20f);
            strokeRect(
                canvas,
                box,
                active ? Color{1.f, 1.f, 1.f, 1.f} : Color{0.78f, 0.78f, 0.78f, 1.f},
                active ? 0.95f : 0.8f
            );
            canvas.text(
                box.x0,
                textTopIn(box.y0, rowHeight),
                box.x1 - box.x0,
                mEnabled ? tr("Shaders: Enabled") : tr("Shaders: Disabled"),
                textColor,
                Align::Center,
                1.f
            );
        }
    }

    int const first   = listFirstVisible();
    int const visible = std::max(L.rowCount - 1, 1);
    for (int k = 0; k < visible && first + k < (int)packs.size(); ++k) {
        int const   packIndex = first + k;
        auto const& r         = L.rows[1 + k];
        bool const  sel       = mRow == packIndex + 1;
        bool const  hov       = r.contains(mMouseX, mMouseY);

        drawRowHighlight(canvas, r, sel, hov);
        canvas.text(
            r.x0,
            textTopIn(r.y0, rowHeight),
            r.x1 - r.x0,
            packLabel(packs[(std::size_t)packIndex]),
            sel ? selectColor : (hov ? textColor : dimColor),
            Align::Center,
            1.f
        );
    }

    if (packs.empty()) {
        canvas.text(0.f, L.contentTop + rowHeight, width, tr("No shader packs found"), dimColor, Align::Center, 1.f);
    }
    // 拖放提示（Iris 的 “+ 拖放光影包至此以添加”）：固定贴在中间栏最底部那一行，
    // 不再跟着最后一个包浮动 —— 列表因此能吃满整栏，一屏能看到更多包。
    canvas.text(0.f, textTopIn(L.hintTop, rowHeight), width, "+ " + tr("Drag and drop shader packs here to add"),
                dimColor, Align::Center, 1.f);

    // 右侧滚动条：包一屏放不下时出现，贴屏幕最右边（滚轮/方向键移动高亮行时会跟着滚）。
    {
        Rect track{}, handle{};
        listScrollBar(width, track, handle);
        drawScrollBar(canvas, track, handle);
    }

    drawStatusLine(canvas, width, L.buttons[0].y0 - 36.f);

    // Iris 在按钮区上方有一条水平分界线，横穿整个屏幕。
    canvas.fill(0.f, L.buttons[0].y0 - 8.f, width, 1.f, barColor, 0.85f);

    std::string const texts[5] = {
        tr("Open shader pack folder..."),
        tr("Shader pack settings..."),
        tr("Cancel"),
        tr("Apply"),
        tr("Done"),
    };
    for (int i = 0; i < L.buttonCount; ++i) drawButton(canvas, L.buttons[i], texts[i], i);
}

// 设置页（Iris 风格）：顶栏标题 + 右上角 导入/导出/重置 + 左上角返回，
// 中间是可滚动的参数列表（两列 / 整行两种版式，滑块选中后放大占满文本框）。
void Screen::drawSettings(Canvas& canvas, float width, float height) {
    drawContentBackdrop(canvas, width);

    // 顶栏：和列表页同一套观感（两行居中标题 + 底下一条贯穿分界线）。
    canvas.fill(0.f, 0.f, width, chromeHeaderLineY, panelColor, chromeAlpha);
    canvas.text(0.f, headerTextY, width, mSettings.title, textColor, Align::Center, 1.f);
    canvas.text(0.f, headerTextY + 14.f, width, tr(mSettings.subtitle), dimColor, Align::Center, 1.f);
    canvas.fill(0.f, chromeHeaderLineY, width, 1.f, barColor, 0.85f);

    SettingsLayout const& S = mSettingsLayout;

    // 顶栏小按钮：常态灰框白字，悬停/焦点 = 白框 + 淡白填充（和底栏按钮一致）。
    auto smallButton = [&](Rect const& r, std::string const& value, bool active) {
        if (!r.valid()) return;
        float const w = r.x1 - r.x0;
        float const h = r.y1 - r.y0;
        if (active) canvas.fill(r.x0, r.y0, w, h, buttonFillActive, 0.26f);
        strokeRect(canvas, r, active ? buttonBorderActive : buttonBorder, 0.9f);
        canvas.text(r.x0, textTopIn(r.y0, h), w, value, textColor, Align::Center, 1.f);
    };

    smallButton(S.headerBack, "< " + tr("Back"), mHeaderFocus == 0 || S.headerBack.contains(mMouseX, mMouseY));
    {
        std::string const headerKeys[3] = {"Import", "Export", "Reset"};
        for (int i = 0; i < S.headerCount; ++i) {
            bool const active = mHeaderFocus == i + 1 || S.header[i].contains(mMouseX, mMouseY);
            smallButton(S.header[i], tr(headerKeys[i]), active);
        }
    }

    // 悬停/拖动的那个滑块：名字不画在行里，改由鼠标旁的浮层显示（对齐 Iris）。
    std::string hoveredSliderLabel;
    float       hoveredSliderRowW = 0.f;

    for (int slotIndex : S.visible) {
        auto const& box    = S.slots[(std::size_t)slotIndex];
        auto const& item   = mSettings.items[(std::size_t)box.index];
        bool const  sel    = box.index == mSettingIndex;
        bool const  hov    = box.row_.contains(mMouseX, mMouseY);
        bool const  active = sel || hov;
        float const rowH   = box.row_.y1 - box.row_.y0;
        float const rowW   = box.row_.x1 - box.row_.x0;

        // 顶层分类按钮：整格一个按钮，居中显示分类名，点开进入下一层。
        if (box.button) {
            canvas.fill(box.row_.x0, box.row_.y0, rowW, rowH, Color{0.f, 0.f, 0.f, 1.f}, 0.45f);
            strokeRect(canvas, box.row_, active ? buttonBorderActive : buttonBorder, active ? 0.95f : 0.85f);
            canvas.text(box.row_.x0, textTopIn(box.row_.y0, rowH), rowW, tr(item.label),
                        active ? selectColor : textColor, Align::Center, 1.f);
            continue;
        }

        // 分类标题（理论上只在旧结构里出现）：整行一条深色带。
        if (item.kind == SettingKind::Category) {
            canvas.fill(box.row_.x0, box.row_.y0, rowW, rowH, Color{0.f, 0.f, 0.f, 1.f}, 0.28f);
            strokeRect(canvas, box.row_, buttonBorder, 0.8f);
            canvas.text(box.row_.x0, textTopIn(box.row_.y0, rowH), rowW, tr(item.label), textColor, Align::Center,
                        1.f);
            continue;
        }

        // 每个参数 = 一个文本框：和底栏五个按钮同款（常态灰框，悬停/选中白框）。
        // 底色对所有行统一（和复选框那行保持一致）；悬停/选中只改框线和名字颜色。
        // 展开的滑块整格铺成值框那种黑底，所以它和复选框的黑底是同一块颜色。
        canvas.fill(box.row_.x0, box.row_.y0, rowW, rowH, Color{0.f, 0.f, 0.f, 1.f}, box.expanded ? 0.9f : 0.45f);
        strokeRect(canvas, box.row_, active ? buttonBorderActive : buttonBorder, active ? 0.95f : 0.85f);

        if (box.expanded) {
            // 悬停/拖动：整格变成滑块轨道（对齐 Iris 的 SliderElementWidget.renderSlider）——
            // 黑色轨道 + 落在数值位置的原版按钮小块滑标 + 居中的数值；
            // 参数名这一步**不画**，改由鼠标旁那个独立浮层显示（见本函数末尾）。
            std::string const value = settingsValueText(item, tr("ON"), tr("OFF"));
            // 数值画在轨道正中（Iris 的 renderSlider 就是这样），滑标可能正好压在数字上，
            // 所以照 Iris 的样子带一层 1px 黑影，保证任何位置都读得清。
            canvas.text(box.row_.x0 + 1.f, textTopIn(box.row_.y0, rowH) + 1.f, rowW, value,
                        Color{0.f, 0.f, 0.f, 1.f}, Align::Center, 1.f);
            canvas.text(box.row_.x0, textTopIn(box.row_.y0, rowH), rowW, value, valueColor, Align::Center, 1.f);

            Rect const thumb = sliderThumbRect(box.row_, settingsSliderFraction(item));
            drawSliderThumb(canvas, thumb.x0, thumb.y0, thumb.x1 - thumb.x0, thumb.y1 - thumb.y0, mSettingDragging);

            hoveredSliderLabel = tr(item.label);
            hoveredSliderRowW  = rowW;
            continue;
        }

        // 名字在左：常态白字、悬停/选中黄字（Iris 用黄色标出当前项）。
        canvas.text(box.row_.x0 + 5.f, textTopIn(box.row_.y0, rowH), box.control.x0 - box.row_.x0 - 7.f,
                    tr(item.label), active ? selectColor : textColor, Align::Left, 1.f);

        // 值框在右：黑底 + 亮框，和 Iris 的数值框一致。
        Rect const& inner  = box.control;
        float const  innerH = inner.y1 - inner.y0;
        float const  innerW = inner.x1 - inner.x0;
        if (item.kind != SettingKind::Color) {
            canvas.fill(inner.x0, inner.y0, innerW, innerH, Color{0.f, 0.f, 0.f, 1.f}, 0.88f);
        }

        switch (item.kind) {
        case SettingKind::Bool:
            strokeRect(canvas, inner, active ? buttonBorderActive : innerBorderColor, 0.95f);
            canvas.text(inner.x0, textTopIn(inner.y0, innerH), innerW, settingsValueText(item, tr("ON"), tr("OFF")),
                        textColor, Align::Center, 1.f);
            break;
        case SettingKind::Enum:
            strokeRect(canvas, inner, active ? buttonBorderActive : innerBorderColor, 0.95f);
            canvas.text(inner.x0, textTopIn(inner.y0, innerH), innerW, tr(settingsEnumLabel(item)), valueColor,
                        Align::Center, 1.f);
            break;
        case SettingKind::Color:
            canvas.fill(inner.x0, inner.y0, innerW, innerH, parseHexColor(item.value), 1.f);
            strokeRect(canvas, inner, active ? buttonBorderActive : innerBorderColor, 0.95f);
            break;
        case SettingKind::Slider: {
            strokeRect(canvas, inner, active ? buttonBorderActive : innerBorderColor, 0.95f);
            std::string const value = settingsValueText(item, tr("ON"), tr("OFF"));
            canvas.text(inner.x0, textTopIn(inner.y0, innerH), innerW, value, valueColor, Align::Center, 1.f);
            // 紧凑态：值框里有一条 4 单位宽的滑标预览（Iris 的 PREVIEW_SLIDER_WIDTH），
            // 位置跟着当前值走 —— 和展开态一样是"原版按钮小块"，不是白线。
            constexpr float previewThumbW = 4.f;
            float const     spanX0        = inner.x0 + 2.f;
            float const     spanX1        = inner.x1 - 2.f - previewThumbW;
            float const     tx = spanX0 + std::max(spanX1 - spanX0, 0.f) * settingsSliderFraction(item);
            drawSliderThumb(canvas, tx, inner.y0 + sliderThumbInsetY, previewThumbW,
                            innerH - 2.f * sliderThumbInsetY, false);
            break;
        }
        default:
            break;
        }
    }

    // Iris 的“名字跟着鼠标走”的浮层（GuiUtil.drawTextPanel）：拖动/悬停滑块时，参数名是一个
    // **独立的文本框** —— 紧贴鼠标右上、可以越出所在那一行（只收在屏幕边缘），
    // 宽度按文字量算，最宽不超过那一行。画在所有参数行之后，保证压在最上层。
    if (!hoveredSliderLabel.empty()) {
        // 关键：先把前面所有矩形/文字提交出去，浮层才能盖在它们（尤其是数值）上面。
        // 游戏里的画布是"所有填充 → 所有文字"两段式提交的，不 flush 的话浮层的黑底会被
        // 后面的数值文字压过去（Iris 用的是 TOP_LAYER_RENDER_QUEUE，等价于这里的一次 flush）。
        canvas.flush();
        float const tw = canvas.textWidth(hoveredSliderLabel, 1.f);
        float const w  = std::min(tw + 8.f, std::max(hoveredSliderRowW, 24.f));
        float const h  = 16.f;
        float       x  = mMouseX + 2.f;
        float       y  = mMouseY - h;
        x              = std::clamp(x, 1.f, std::max(width - w - 1.f, 1.f));
        y              = std::clamp(y, 1.f, std::max(height - h - 1.f, 1.f));
        canvas.fill(x, y, w, h, Color{0.f, 0.f, 0.f, 1.f}, 0.87f);
        strokeRect(canvas, Rect{x, y, x + w, y + h}, Color{0.87f, 0.87f, 0.87f, 1.f}, 0.95f);
        canvas.text(x + 4.f, textTopIn(y, h), w - 8.f, hoveredSliderLabel, textColor, Align::Center, 1.f);
    }

    // 右侧滚动条（内容超出可视区时才画；贴屏幕最右边）。
    drawScrollBar(canvas, S.scrollTrack, S.scrollHandle);

    // 空状态：这个包（或当前分类）没有声明任何可调参数。
    if (mSettingsView.empty()) {
        canvas.text(
            0.f,
            S.viewport.y0 + 20.f,
            width,
            tr("This shader pack has no configurable options"),
            dimColor,
            Align::Center,
            1.f
        );
    }

    // 状态提示：贴在可视区底部的一条“toast”，避免和可滚动的参数行抢位置。
    // 底色**透明**；显示满 3 秒后开始淡出，第 4 秒整消失（见 statusAlpha）。
    {
        std::string const message = effectiveStatus();
        float const       alpha   = statusAlpha(message);
        if (!message.empty() && alpha > 0.02f) {
            Color      color  = mNoticeFailed ? failColor : accentColor;
            color.a           = alpha;
            Rect const bar{S.viewport.x0, S.viewport.y1 - 15.f, S.viewport.x1, S.viewport.y1};
            canvas.text(bar.x0, textTopIn(bar.y0, 15.f), bar.x1 - bar.x0, message, color, Align::Center, 1.f);
        }
    }
    canvas.fill(0.f, chromeBottomBarTop(height), width, 1.f, barColor, 0.85f);

    std::string const texts[5] = {
        tr("Open shader pack folder..."),
        tr("Shader pack list..."),
        tr("Cancel"),
        tr("Apply"),
        tr("Done"),
    };
    for (int i = 0; i < mLayout.buttonCount; ++i) drawButton(canvas, mLayout.buttons[i], texts[i], i);
}

void Screen::render(Canvas& canvas, float width, float height) {
    if (!mVisible) return;
    if (width <= 0.f || height <= 0.f) return;

    mLayout = buildLayout(width, height);
    mLastWidth  = width;
    mLastHeight = height;

    // 设置页版式：每帧重建。先用"没有悬停"的版式定位鼠标下的项（格子几何和悬停无关），
    // 再带上悬停项重建 —— 滑块只要鼠标移上去就展开、一移开立刻复原。
    if (mMode == Mode::Settings) {
        // -1 = 顶层分类页（按钮网格）；-2 = 包没有分类时的"整页参数"；>=0 = 某个分类页。
        SettingPageStyle const style = (mSettingsPage == -1) ? SettingPageStyle::ButtonGrid : SettingPageStyle::Rows;
        mSettingsLayout = buildSettingsLayout(width, height, mSettings, mSettingsView, style, -1, mSettingScroll);
        mSettingScroll  = mSettingsLayout.scroll;
        mSettingsHover  = settingsHitTest(mSettingsLayout, mMouseX, mMouseY);
        if (mSettingsHover >= 0) {
            mSettingsLayout =
                buildSettingsLayout(width, height, mSettings, mSettingsView, style, mSettingsHover, mSettingScroll);
        }
    }

    // 底部状态提示的淡出计时：文字一变就重新计时（见 statusAlpha）。
    trackStatus(effectiveStatus());

    // 透明预览：除了右下角按钮，其他一切都不画。
    if (mTransparentPreview) {
        drawPreviewButton(canvas);
        canvas.flush();
        return;
    }

    // 编译结束：执行「完成」排队的动作。
    if (mPending == Pending::BackToList) {
        mPending = Pending::None;
        open(); // 重新记录快照（open 里会回到列表页）
    }

    // 底栏（按钮区）先铺一层比中间内容区更透明的底，再往上画按钮。
    drawBottomBar(canvas, width, height);

    switch (mMode) {
    case Mode::List:
        drawList(canvas, width);
        break;
    case Mode::Settings:
        drawSettings(canvas, width, height);
        break;
    }

    drawPreviewButton(canvas);
    canvas.flush();
}

// ---------------------------------------------------------------- 交互

void Screen::selectRow(int delta) {
    if (mMode != Mode::List) return; // 设置页的选择走动走 handleSettingsKey
    auto const& packs = ShaderPackStore::getInstance().packs();
    // 列表页的条目 = 1 个"光影：已启用/禁用"开关 + N 个包。
    int count = 1 + (int)packs.size();
    if (count <= 0) return;
    mRow = (mRow + delta) % count;
    if (mRow < 0) mRow += count;
    listSyncToRow();
}

int Screen::contentCount() const {
    if (mMode != Mode::List) return 0;
    auto const& packs = ShaderPackStore::getInstance().packs();
    return 1 + (int)packs.size(); // 第 0 项是总开关
}

// 列表页能滚到的最靠后的"第一个可见包"（包没满一屏就是 0，不滚动）。
int Screen::listMaxFirst() const {
    auto const& packs     = ShaderPackStore::getInstance().packs();
    int const   packCount = (int)packs.size();
    int const   visible   = std::max(mLayout.rowCount - 1, 1);
    return std::max(0, packCount - visible);
}

// 可视窗口的第一个包。滚轮 / 拖滚动条只动它，**不碰选中行**
// （用户要求：滚轮只能滚动，能选中东西的只有鼠标所在的地方）。
int Screen::listFirstVisible() const { return std::clamp(mListFirst, 0, listMaxFirst()); }

// 键盘移动选中行之后，把可视窗口挪到能看见它。鼠标点击不需要（点的那一行本来就在眼皮底下）。
void Screen::listSyncToRow() {
    if (mRow <= 0) return; // 第 0 行是总开关，永远在顶上
    if (mLayout.rowCount <= 0) return; // 还没渲染过：没有可视窗口可算
    int const visible = std::max(mLayout.rowCount - 1, 1);
    int const target  = mRow - 1; // 换算成包下标
    int       first   = listFirstVisible();
    if (target < first) {
        first = target;
    } else if (target >= first + visible) {
        first = target - visible + 1;
    }
    mListFirst = std::clamp(first, 0, listMaxFirst());
}

void Screen::listScrollBar(float width, Rect& track, Rect& handle) const {
    track  = {};
    handle = {};
    auto const& packs     = ShaderPackStore::getInstance().packs();
    int const   packCount = (int)packs.size();
    int const   visible   = std::max(mLayout.rowCount - 1, 1);
    if (packCount <= visible) return;

    float const top     = mLayout.contentTop;
    float const bottom  = std::max(mLayout.hintTop - 4.f, top + 18.f); // 停在拖放提示那一行之上
    float const trackH  = bottom - top;
    float const handleH = std::max(18.f, trackH * (float)visible / (float)packCount);
    int const   maxFirst = listMaxFirst();
    float const t        = (maxFirst > 0) ? (float)listFirstVisible() / (float)maxFirst : 0.f;
    track = Rect{width - 7.f, top, width - 1.f, bottom};
    float const hy0 = top + t * (trackH - handleH);
    handle = Rect{track.x0, hy0, track.x1, hy0 + handleH};
}

// 可见行 -> 列表项下标（0 = 总开关，k = 第 k-1 个包）；没有对应项返回 -1。
int Screen::listIndexAtRow(int row) const {
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (row == 0) return 0;
    if (row < 0 || row >= mLayout.rowCount) return -1;
    int const idx = listFirstVisible() + (row - 1);
    return (idx >= 0 && idx < (int)packs.size()) ? idx + 1 : -1;
}

// 「光影：已启用/已禁用」开关：**只改暂存状态**（标签立刻变），真正加载要等「应用/完成」。
void Screen::toggleShaders() { mEnabled = !mEnabled; }

void Screen::snapshotParameters() {
    auto const& packs = ShaderPackStore::getInstance().packs();
    mSnapshot.clear();
    mSnapshotPack.clear();
    if (mPack >= 0 && (std::size_t)mPack < packs.size()) {
        mSnapshotPack = packs[(std::size_t)mPack].name;
        for (auto const& p : packs[(std::size_t)mPack].parameters) mSnapshot += p.name + "=" + p.current + "\n";
    }
}

// 有没有"还没落地的改动"：开关状态、选中的包、参数值，任一个和当前已生效的不一致就算有。
bool Screen::pendingChanged() const {
    if (mEnabled != mEnabledApplied) return true;
    // 只有"开着"的时候换包才算改动；关着的时候选哪个包无所谓（还没启用）。
    if (mEnabled && mPack != mAppliedPack) return true;
    return dirty();
}

// 把暂存的改动落地，「应用」和「完成」都走这里。
//   开关/选包变了 → 真的加载（或还原原版）；参数变了 → 存盘（编译暂停，不编译）。
// 返回 false = 这次没能应用（例如选中的包没有编译产物），调用方应留在界面上让玩家看到提示。
bool Screen::applyPending() {
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (!pendingChanged()) return true; // 没有任何改动：点「应用」什么都不做

    bool const paramsChanged = dirty();
    if (paramsChanged && mPack >= 0 && (std::size_t)mPack < packs.size()) {
        ShaderPackStore::saveSettings(packs[(std::size_t)mPack]);
    }

    bool const stateChanged = (mEnabled != mEnabledApplied) || (mEnabled && mPack != mAppliedPack);
    if (stateChanged) {
        if (!mEnabled) {
            if (mHost.restoreVanilla) mHost.restoreVanilla();
            mAppliedPack    = -1;
            mEnabledApplied = false;
            mNotice.clear();
            log("[ui] shaders disabled");
        } else {
            if (mPack < 0 || (std::size_t)mPack >= packs.size()) {
                notice(tr("Select a shader pack first"), true);
                return false;
            }
            auto const& pack = packs[(std::size_t)mPack];
            if (countMaterialBins(pack.binsDir) == 0) {
                // 编译已暂停（load-only）：这里不触发编译，直接告诉玩家缺什么。
                log("[ui] '" + pack.name + "' has no compiled bins; compiling is paused");
                notice(tr("This shader pack has no compiled output yet"), true);
                return false;
            }
            if (mHost.reload) mHost.reload(pack);
            mAppliedPack    = mPack;
            mEnabledApplied = true;
            mNotice.clear();
            log("[ui] shaders enabled: " + pack.name);
        }
    }

    if (paramsChanged) notice(tr("Settings saved (compiling is paused)"), false);
    snapshotParameters(); // 应用完就不再有"改动"
    return true;
}

// 「取消 / Esc」：丢掉所有没落地的改动（开关、选包、参数），回到已生效的状态。
void Screen::discardPending(bool refresh) {
    bool const hadPending = pendingChanged();
    mEnabled              = mEnabledApplied;
    if (mAppliedPack >= 0) mPack = mAppliedPack;
    mNotice.clear();
    if (hadPending && refresh) refreshPacks(); // 参数值从 .txt 重新读回来（没保存过的编辑就丢了）
    snapshotParameters();
}

void Screen::adjustSelected(int delta) {
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) return;
    auto const& pack = packs[(std::size_t)mPack];
    if (mRow < 0 || (std::size_t)mRow >= pack.parameters.size()) return;

    auto const& p = pack.parameters[(std::size_t)mRow];
    std::string next;

    switch (p.type) {
    case ParameterType::Bool:
        next = isOnValue(p.current) ? "OFF" : "ON";
        break;
    case ParameterType::Slider: {
        double cur  = parseNumber(p.current, p.def);
        double step = p.integer ? 1.0 : std::max((p.max - p.min) / 20.0, 0.001);
        double v    = cur + step * (double)delta;
        next        = formatNumber(std::clamp(v, std::min(p.min, p.max), std::max(p.min, p.max)), p.integer);
        break;
    }
    case ParameterType::Enum: {
        if (p.values.empty()) return;
        auto it  = std::find(p.values.begin(), p.values.end(), p.current);
        int  idx = it == p.values.end() ? 0 : (int)std::distance(p.values.begin(), it);
        idx      = (int)((idx + delta) % (int)p.values.size());
        if (idx < 0) idx += (int)p.values.size();
        next = p.values[(std::size_t)idx];
        break;
    }
    case ParameterType::Color:
        return;
    }

    ShaderPackStore::getInstance().setParameterValue(pack.name, (std::size_t)mRow, std::move(next));
}

// ---------------------------------------------------------------- 设置页
// Iris 风格自定义参数界面：参数来自光影包**真实声明**的 shaders.properties（option.*）。
// 包没声明任何参数时显示空状态提示；改完的值写进 <包>.txt（编译链路打开后才参与编译）。

void Screen::enterSettings() {
    mMode = Mode::Settings;

    auto const& packs   = ShaderPackStore::getInstance().packs();
    bool const  hasPack = (mPack >= 0 && (std::size_t)mPack < packs.size());
    mSettings           = hasPack ? settingsFromPack(packs[(std::size_t)mPack]) : SettingsModel{};
    mSettings.title     = hasPack ? packLabel(packs[(std::size_t)mPack]) : tr("Shader Packs");
    mSettings.subtitle  = "Configure";

    // 有分类（screen.*）就先进分类页（Iris 的第一层）；没有分类就直接给一页参数。
    std::vector<int> const top = settingsTopLevelView(mSettings);
    if (!top.empty()) {
        mSettingsPage = -1;
        mSettingsView = top;
    } else {
        mSettingsPage = -2; // -2 = "整页参数"（没有分类页可返回）
        mSettingsView = settingsFlatView(mSettings);
    }
    mSettingIndex    = settingsFirstSelectable(mSettings, mSettingsView);
    mSettingScroll   = 0.f;
    mSettingDragging = false;
    mHeaderFocus     = -1;
    mButtonFocus     = -1;
    mNotice.clear();

    // 进设置页时的值快照：判断有没有改动 / 「取消」时回退。
    mSettingsSnapshot = dumpSettings(mSettings);
    // 用最近一帧的屏幕尺寸立刻算出版式：这样进入设置页后的第一次点击就能命中。
    SettingPageStyle const style =
        (mSettingsPage == -1) ? SettingPageStyle::ButtonGrid : SettingPageStyle::Rows;
    mSettingsLayout = buildSettingsLayout(mLastWidth, mLastHeight, mSettings, mSettingsView, style, -1, mSettingScroll);
    log("[ui] settings page: " + std::to_string(mSettings.items.size()) + " item(s)");
}

// 打开某个分类：下一层显示该分类下的参数，顶栏「返回」回到上一层。
void Screen::openSettingsCategory(int categoryIndex) {
    if (categoryIndex < 0 || (std::size_t)categoryIndex >= mSettings.items.size()) return;
    mSettingsPage    = categoryIndex;
    mSettingsView    = settingsCategoryView(mSettings, categoryIndex);
    mSettingIndex    = settingsFirstSelectable(mSettings, mSettingsView);
    mSettingScroll   = 0.f;
    mSettingDragging = false;
    mHeaderFocus     = -1;
    mSettingsLayout  = buildSettingsLayout(mLastWidth, mLastHeight, mSettings, mSettingsView,
                                           SettingPageStyle::Rows, -1, mSettingScroll);
    log("[ui] settings category: " + settingsCategoryLabel(mSettings, categoryIndex));
}

// 「返回」：先在设置页内部逐层往上，已经在顶层就退回光影包列表。
void Screen::settingsGoBack() {
    if (mSettingsPage < 0) {
        leaveSettings();
        return;
    }
    int const previous = mSettingsPage;
    mSettingsPage    = -1;
    mSettingsView    = settingsTopLevelView(mSettings);
    mSettingIndex    = previous; // 回到刚才那个分类上
    mSettingScroll   = 0.f;
    mSettingDragging = false;
    mSettingsLayout  = buildSettingsLayout(mLastWidth, mLastHeight, mSettings, mSettingsView,
                                           SettingPageStyle::ButtonGrid, -1, mSettingScroll);
}

void Screen::leaveSettings() {
    mMode            = Mode::List;
    mRow             = mPack >= 0 ? mPack + 1 : 0;
    mButtonFocus     = -1;
    mHeaderFocus     = -1;
    mSettingDragging = false;
}

bool Screen::settingsDirty() const { return dumpSettings(mSettings) != mSettingsSnapshot; }

void Screen::applySettingsEdits() {
    if (!settingsDirty()) return; // 没有改动：点「应用」什么都不做（和列表页一致）
    // 编译暂停中：这里只把值记下来（占位），不触发任何编译。
    mSettingsSnapshot = dumpSettings(mSettings);
    notice(tr("Settings saved (compiling is paused)"), false);
    log("[ui] settings applied: " + std::to_string(mSettings.items.size()) + " item(s)");
}

void Screen::discardSettingsEdits() {
    if (mSettingsSnapshot.empty()) return;
    std::string const hay = "\n" + mSettingsSnapshot; // 加个前缀，让首行也能被匹配到
    for (auto& item : mSettings.items) {
        if (item.kind == SettingKind::Category) continue;
        std::string const token = "\n" + item.key + "=";
        auto const        pos   = hay.find(token);
        if (pos == std::string::npos) continue;
        auto const begin = pos + token.size();
        auto       end   = hay.find('\n', begin);
        if (end == std::string::npos) end = hay.size();
        item.value = hay.substr(begin, end - begin);
    }
}

// 建议文件名：和模组自己保存的 <shaderpacks>/<包名>.txt 对齐（对标 Iris 的 shaderpack 配置）。
std::string Screen::suggestedSettingsFileName() const {
    std::string name = mSettings.title.empty() ? std::string("shaderpack") : mSettings.title;
    return name + ".txt";
}

// 把一份 "key=value" 文本应用到当前模型（格式和 <包名>.txt 一致：# 与 ; 开头的行是注释）。
// 返回成功应用的条数；滑块值会被夹回自己的 min..max。
int Screen::applySettingsText(std::string const& text) {
    auto trim = [](std::string s) {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    };

    int         applied = 0;
    std::size_t pos     = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos              = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        std::size_t const eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string const key = trim(line.substr(0, eq));
        std::string const val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        for (auto& item : mSettings.items) {
            if (item.kind == SettingKind::Category || item.key != key) continue;
            if (item.kind == SettingKind::Slider) {
                double const lo = std::min(item.min, item.max);
                double const hi = std::max(item.min, item.max);
                double       v  = parseNumber(val, item.def);
                v               = std::clamp(v, lo, hi);
                if (item.integer) v = std::llround(v);
                item.value = formatNumber(v, item.integer);
            } else {
                item.value = val;
            }
            ++applied;
            break;
        }
    }
    return applied;
}

// 顶栏「导入」：宿主弹文件对话框选一份配置，读进来后逐项回填。
void Screen::importSettingsFile() {
    if (!mHost.importSettings) return;
    std::string text;
    if (!mHost.importSettings(suggestedSettingsFileName(), text)) {
        notice(tr("Import cancelled"), true);
        return;
    }
    int const applied = applySettingsText(text);
    if (applied <= 0) {
        notice(tr("No matching settings in the imported file"), true);
        return;
    }
    notice(std::to_string(applied) + " " + tr("settings imported"), false);
    log("[ui] settings imported: " + std::to_string(applied) + " item(s)");
}

// 顶栏「导出」：把当前的值序列化成 key=value，交给宿主写到用户挑的位置。
void Screen::exportSettingsFile() {
    if (!mHost.exportSettings) return;
    std::string const text = "# RenderDragonShaderLoader 导出的选项值\n" + dumpSettings(mSettings);
    if (!mHost.exportSettings(suggestedSettingsFileName(), text)) {
        notice(tr("Export cancelled"), true);
        return;
    }
    notice(tr("Settings exported"), false);
    log("[ui] settings exported: " + std::to_string(mSettings.items.size()) + " item(s)");
}

void Screen::clampSettingScroll() {
    SettingsLayout const& S = mSettingsLayout;
    for (auto const& box : S.slots) {
        if (box.index != mSettingIndex) continue;
        if (box.row_.y0 < S.viewport.y0) {
            mSettingScroll -= (S.viewport.y0 - box.row_.y0);
        } else if (box.row_.y1 > S.viewport.y1) {
            mSettingScroll += (box.row_.y1 - S.viewport.y1);
        }
        break;
    }
    mSettingScroll = std::clamp(mSettingScroll, 0.f, std::max(S.maxScroll, 0.f));
}

// 拖动滑块：按指针 x 落在轨道上的比例取值。轨道映射和绘制滑标用的是同一个函数
// （sliderFractionAt），所以滑标永远跟在指针下面。
void Screen::updateSettingDrag(float x, float y) {
    (void)y;
    for (auto const& box : mSettingsLayout.slots) {
        if (box.index != mSettingIndex) continue;
        if (mSettings.items[(std::size_t)box.index].kind != SettingKind::Slider) return;
        settingsSetSlider(mSettings, mSettingIndex, sliderFractionAt(box.row_, x));
        clampSettingScroll();
        return;
    }
}

// 按住滚动条上下拖动（列表页 / 设置页共用）。按下时记下的 grabDy 保证滑块不跳。
void Screen::updateScrollDrags(float y) {
    if (mSettingScrollDragging) {
        SettingsLayout const& S = mSettingsLayout;
        if (S.maxScroll > 0.f && S.scrollTrack.valid() && S.scrollHandle.valid()) {
            mSettingScroll = scrollFromHandle(y, mScrollGrabDy, S.scrollTrack, S.scrollHandle, S.maxScroll);
        }
    }
    if (mListScrollDragging) {
        Rect track{}, handle{};
        listScrollBar(mLastWidth, track, handle);
        int const maxFirst = listMaxFirst();
        if (track.valid() && handle.valid() && maxFirst > 0) {
            float const t = scrollFromHandle(y, mListGrabDy, track, handle, (float)maxFirst);
            mListFirst    = std::clamp((int)std::lround(t), 0, maxFirst);
        }
    }
}

void Screen::moveSettingSelection(int dx, int dy) {
    int const next = settingsMove(mSettingsLayout, mSettingIndex, dx, dy);
    if (next >= 0 && next != mSettingIndex) {
        mSettingIndex = next;
        clampSettingScroll();
        return;
    }
    // 左右没有相邻列时：**只有滑块**把左右键当成微调（Iris 也是这样）；
    // bool / enum / 颜色改用 Enter 或 [ ]，免得方向键莫名其妙把开关翻掉。
    if (dx != 0 && mSettingIndex >= 0 && (std::size_t)mSettingIndex < mSettings.items.size() &&
        mSettings.items[(std::size_t)mSettingIndex].kind == SettingKind::Slider) {
        settingsAdjust(mSettings, mSettingIndex, dx);
    }
}

bool Screen::handleSettingsKey(int keyCode) {
    // 焦点在底栏按钮上：沿用和列表页同一套按钮导航。
    if (mButtonFocus >= 0) {
        switch (keyCode) {
        case 0x26: mButtonFocus = (mButtonFocus >= 2) ? std::max(mButtonFocus - 2, 0) : -1; return true;
        case 0x28: if (mButtonFocus < 2) mButtonFocus = std::min(mButtonFocus + 2, 4); return true;
        case 0x25: mButtonFocus = (mButtonFocus >= 2) ? std::max(mButtonFocus - 1, 2) : 0; return true;
        case 0x27: mButtonFocus = (mButtonFocus >= 2) ? std::min(mButtonFocus + 1, 4) : 1; return true;
        case 0x0D:
            click();
            activateButton(mButtonFocus);
            return true;
        case 0x1B: settingsGoBack(); return true;
        default:   return true;
        }
    }

    switch (keyCode) {
    case 0x1B: // Esc = 「返回」：先回上一层，顶层再回光影包列表
        settingsGoBack();
        return true;
    case 0x26: // Up
        moveSettingSelection(0, -1);
        return true;
    case 0x28: { // Down：到底了就交给底栏按钮
        int const before = mSettingIndex;
        moveSettingSelection(0, 1);
        if (mSettingIndex == before) mButtonFocus = 2;
        return true;
    }
    case 0x25: // Left
        moveSettingSelection(-1, 0);
        return true;
    case 0x27: // Right
        moveSettingSelection(1, 0);
        return true;
    case 0x0D: // Enter：分类按钮=打开下一层；参数=改值（bool 取反 / enum 循环 / color 换色）
        if (mSettingIndex >= 0 && (std::size_t)mSettingIndex < mSettings.items.size() &&
            mSettings.items[(std::size_t)mSettingIndex].kind == SettingKind::Category) {
            click();
            openSettingsCategory(mSettingIndex);
        } else if (mSettingIndex >= 0) {
            click();
            settingsAdjust(mSettings, mSettingIndex, 1);
        }
        return true;
    case 0xDB: // [
        if (mSettingIndex >= 0) {
            click();
            settingsAdjust(mSettings, mSettingIndex, -1);
        }
        return true;
    case 0xDD: // ]
        if (mSettingIndex >= 0) {
            click();
            settingsAdjust(mSettings, mSettingIndex, 1);
        }
        return true;
    case 0x09: // Tab：下一个设置项
        moveSettingSelection(0, 1);
        return true;
    default:
        return true;
    }
}

void Screen::activateRow(int index) {
    if (mMode != Mode::List) return;
    auto const& packs = ShaderPackStore::getInstance().packs();
    // 第 0 项 = "光影：已启用/已禁用"总开关，直接切换（和 Iris 一样）。
    if (index == 0) {
        mRow = 0;
        toggleShaders(); // 只改暂存状态，等「应用/完成」才真的开关光影
        return;
    }
    int const packIndex = index - 1;
    if (packIndex < 0 || (std::size_t)packIndex >= packs.size()) return;
    // 选中一个光影 = 打算用它（开关显示"已启用"），但**先不加载**，等「应用/完成」。
    mPack        = packIndex;
    mRow         = index;
    mButtonFocus = -1;
    mEnabled     = true;
    snapshotParameters(); // 快照换成这个包的参数，之后调参才算"改动"
}

void Screen::activateButton(int index) {
    switch (index) {
    case 0: // 打开光影包文件夹
        if (mHost.openFolder) mHost.openFolder();
        return;
    case 1: // 光影包设置... / 光影包列表...
        if (mMode == Mode::List) {
            enterSettings();
        } else {
            leaveSettings();
        }
        return;
    case 2: // 取消：设置页 = 回列表；列表页 = 丢掉改动并退出
        if (mMode == Mode::Settings) {
            discardSettingsEdits();
            leaveSettings();
            return;
        }
        discardPending();
        close();
        return;
    case 3: // 应用：把暂存的改动落地（加载/还原/存盘），界面保持不变
        if (mMode == Mode::Settings) {
            applySettingsEdits();
            return;
        }
        applyPending();
        return;
    case 4: // 完成 = 应用 + 退出面板。**没有改动 / 不用编译也要关掉**
        // （用户反馈：编译暂停时点「完成」没反应，面板应该照样关）。
        if (mMode == Mode::Settings) {
            applySettingsEdits();
            close();
            return;
        }
        applyPending(); // 有改动就落地（编译暂停时只提示），没改动也继续往下关面板
        close();
        return;
    default:
        return;
    }
}

bool Screen::dirty() const {
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) return false;
    auto const& pack = packs[(std::size_t)mPack];
    if (pack.name != mSnapshotPack) return false;

    std::string current;
    for (auto const& p : pack.parameters) current += p.name + "=" + p.current + "\n";
    return current != mSnapshot;
}

bool Screen::handleKey(int keyCode) {
    if (!mVisible) return false;

    // 透明预览时键盘也一概不响应，避免误操作；只保留右下角按钮。
    if (mTransparentPreview) return true;

    // 编译中锁住界面。
    if (blocked()) return true;

    log("[ui] key " + std::to_string(keyCode) + " mode=" + std::to_string((int)mMode) + " row=" + std::to_string(mRow)
        + " rowCount=" + std::to_string(mLayout.rowCount));

    // 设置页有自己的一套选择 / 版式导航。
    if (mMode == Mode::Settings) return handleSettingsKey(keyCode);

    switch (keyCode) {
    case 0x1B:            // Escape
        discardPending(); // 和「取消」一样：丢掉没应用的改动
        close();
        return true;
    case 0x26: // Up
        if (mButtonFocus >= 0) {
            mButtonFocus = (mButtonFocus >= 2) ? std::max(mButtonFocus - 2, 0) : -1;
        } else if (mRow > 0) {
            --mRow;
            listSyncToRow(); // 键盘移动选中行：把可观窗口跟着挪过去（滚轮不会这么做）
        }
        return true;
    case 0x28: // Down
        if (mButtonFocus >= 0) {
            if (mButtonFocus < 2) mButtonFocus = std::min(mButtonFocus + 2, 4);
        } else {
            int count = contentCount();
            if (mRow + 1 < count) {
                ++mRow;
                listSyncToRow();
            } else {
                mButtonFocus = 2; // 到底了：焦点交给下排第一个按钮
            }
        }
        return true;
    case 0x25: // Left
        if (mButtonFocus >= 2) {
            mButtonFocus = std::max(mButtonFocus - 1, 2);
        } else if (mButtonFocus >= 0) {
            mButtonFocus = 0;
        } else if (mRow > 0) {
            --mRow;
            listSyncToRow();
        }
        return true;
    case 0x27: // Right
        if (mButtonFocus >= 2) {
            mButtonFocus = std::min(mButtonFocus + 1, 4);
        } else if (mButtonFocus >= 0) {
            mButtonFocus = 1;
        } else if (mRow + 1 < contentCount()) {
            ++mRow;
            listSyncToRow();
        }
        return true;
    case 0x0D: // Enter —— 等同鼠标左键：按钮就按按钮，否则执行选中项
        if (mButtonFocus >= 0) {
            click();
            activateButton(mButtonFocus);
        } else {
            click();
            activateRow(mRow);
        }
        return true;
    case 0xDB: // [ —— 减小选中项的值
        click();
        adjustSelected(-1);
        return true;
    case 0xDD: // ] —— 增大选中项的值
        click();
        adjustSelected(1);
        return true;
    case 0x09: // Tab
        selectRow(1);
        return true;
    // 注意：这里【不再】给 1..5 绑定五个按钮。之前绑了，结果在界面打开时按数字键
    // 会直接把「打开光影包文件夹」「应用（编译）」这些动作触发掉，打字也会中招。
    // 五个按钮只用鼠标左键或方向键 + 回车操作。
    default:
        return true; // 界面打开时吃掉所有按键，避免影响游戏
    }
}

// 滚轮：设置页上下滚动。正值 = 向上滚 ⇒ 内容往下走（scroll 减小）。
// 注意游戏里滚轮的符号如果反了，只改这里的正负号即可。
bool Screen::handleWheel(int delta) {
    if (!mVisible) return false;
    if (mTransparentPreview || blocked()) return true;

    // 只取方向：不同鼠标/触控板一格的数值可能是 1，也可能是 120，
    // 直接用数值乘步长会"一下滚到底"。
    int const dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0); // +1 = 向上滚
    if (dir == 0) return true;

    if (mMode == Mode::Settings) {
        // 设置页：滚内容（向上滚 = 内容往下走）。
        constexpr float step = 30.f; // 一格滚轮滚动的 GUI 单位
        mSettingScroll -= (float)dir * step;
        mSettingScroll = std::clamp(mSettingScroll, 0.f, std::max(mSettingsLayout.maxScroll, 0.f));
        log("[ui] wheel raw=" + std::to_string(delta) + " dir=" + std::to_string(dir) + " scroll=" +
            std::to_string((int)mSettingScroll));
        return true;
    }

    // 列表页：滚轮**只滚动可视窗口**，不动选中行 —— 用户要求"能选中的只有鼠标所在的地方"。
    // （向下滚 = 看到更靠后的包 = mListFirst 变大。）
    int const maxFirst = listMaxFirst();
    if (maxFirst > 0) mListFirst = std::clamp(mListFirst - dir, 0, maxFirst);
    log("[ui] wheel raw=" + std::to_string(delta) + " dir=" + std::to_string(dir) + " list first=" +
        std::to_string(mListFirst));
    return true;
}

void Screen::handleMouseMove(float x, float y, bool down) {
    if (!mVisible) return;
    mMouseX    = x;
    mMouseY    = y;
    mMouseDown = down;

    // 游戏里鼠标移动走的是 ActionMove → handleMouseMove，所以"按住左键拖滑块"必须在这里更新；
    // 只在 handleMouseButton 里处理的话，按住拖动会没反应（只有点一下才生效）。
    if (mMode == Mode::Settings && mSettingDragging && down) updateSettingDrag(x, y);
    if ((mSettingScrollDragging || mListScrollDragging) && down) updateScrollDrags(y);
}

bool Screen::handleMouseButton(MouseButton button, float x, float y, bool down) {
    if (!mVisible) return false;
    if (blocked()) return true; // 编译中锁定
    if (button != MouseButton::Left) return true;

    bool const wasDown = mMouseDown;
    mMouseDown         = down;

    // 点击必须以本次鼠标事件坐标为准。渲染回调里的 mMouseX/Y 是上一帧位置，
    // 从底栏快速移到内容区点第一下时会命中旧区域，表现成“要点两下”。
    float const hx = (x >= 0.f && y >= 0.f) ? x : mMouseX;
    float const hy = (x >= 0.f && y >= 0.f) ? y : mMouseY;

    Layout const& L = mLayout;

    // 透明预览模式：只有右下角按钮有效，其它鼠标事件全部吞掉。
    if (mTransparentPreview) {
        if (down && !wasDown && L.previewButton.contains(hx, hy)) {
            mTransparentPreview = false;
            log("[ui] transparent preview off");
            click();
        }
        return true;
    }

    if (down && !wasDown && L.previewButton.contains(hx, hy)) {
        mTransparentPreview = true;
        log("[ui] transparent preview on");
        click();
        return true;
    }

    // 滚动条按住拖动：列表页 / 设置页共用。按下时若落在滑块上就记下"指针相对滑块顶端的偏移"
    // （拖起来不跳），落在轨道空白处就把滑块中心挪到指针处。
    if (down && !wasDown) {
        if (mMode == Mode::Settings) {
            SettingsLayout const& S = mSettingsLayout;
            if (S.maxScroll > 0.f && S.scrollTrack.valid() && S.scrollTrack.contains(hx, hy)) {
                float const handleH = S.scrollHandle.y1 - S.scrollHandle.y0;
                mScrollGrabDy          = S.scrollHandle.contains(hx, hy) ? (hy - S.scrollHandle.y0) : handleH / 2.f;
                mSettingScrollDragging = true;
                mSettingDragging       = false;
                updateScrollDrags(hy);
                log("[ui] settings scrollbar drag begin");
                return true;
            }
        } else if (mMode == Mode::List) {
            Rect track{}, handle{};
            listScrollBar(mLastWidth, track, handle);
            if (track.valid() && track.contains(hx, hy)) {
                float const handleH = handle.y1 - handle.y0;
                mListGrabDy         = handle.contains(hx, hy) ? (hy - handle.y0) : handleH / 2.f;
                mListScrollDragging = true;
                updateScrollDrags(hy);
                log("[ui] list scrollbar drag begin");
                return true;
            }
        }
    }
    if (!down) {
        mSettingScrollDragging = false;
        mListScrollDragging    = false;
    }
    if (mSettingScrollDragging || mListScrollDragging) {
        updateScrollDrags(hy); // 有些平台只把坐标报在 move 里，这里再兜一次
        return true;
    }

    // 设置页：滑块拖动（不要求新按下，方便连续拖到目标数值）。
    if (mMode == Mode::Settings) {
        if (!down) {
            // Iris 在松开滑块时也回一声（onReleased）。
            if (mSettingDragging) click();
            mSettingDragging = false;
            return true;
        }
        if (mSettingDragging) {
            updateSettingDrag(hx, hy);
            return true;
        }
    }

    if (!down) return true;

    // 去抖只针对同一位置的重复上报；用户切换到另一个区域时不能被旧的按下状态吞掉。
    {
        static auto  lastClick = std::chrono::steady_clock::time_point{};
        static float lastX     = -1.f;
        static float lastY     = -1.f;
        auto const   now       = std::chrono::steady_clock::now();
        bool const sameSpot =
            std::abs(hx - lastX) < 2.f && std::abs(hy - lastY) < 2.f;
        if (sameSpot && now - lastClick < std::chrono::milliseconds(200)) return true;
        lastClick = now;
        lastX     = hx;
        lastY     = hy;
    }

    // 按钮
    for (int i = 0; i < L.buttonCount; ++i) {
        if (L.buttons[i].contains(hx, hy)) {
            log("[ui] button " + std::to_string(i) + " clicked");
            mHeaderFocus = -1;
            click();
            activateButton(i);
            return true;
        }
    }

    // 设置页：顶栏（返回 + 导入/导出/重置）→ 参数行 → 滚动条。
    if (mMode == Mode::Settings) {
        SettingsLayout const& S = mSettingsLayout;

        if (S.headerBack.contains(hx, hy)) {
            mHeaderFocus = 0;
            log("[ui] settings back");
            click();
            settingsGoBack(); // 先在设置页内部逐层往上，已经在顶层才回光影包列表
            return true;
        }
        for (int i = 0; i < S.headerCount; ++i) {
            if (!S.header[i].contains(hx, hy)) continue;
            mHeaderFocus = i + 1;
            click();
            if (i == 0) { // 导入配置文件：从外部选一份配置读进来
                importSettingsFile();
            } else if (i == 1) { // 导出配置文件：把当前值写到外部选的位置
                exportSettingsFile();
            } else { // 重置为默认配置
                settingsResetDefaults(mSettings);
                notice(tr("Reset to defaults"), false);
            }
            log("[ui] settings header button " + std::to_string(i) + " clicked");
            return true;
        }

        SettingPart part = SettingPart::None;
        int const   idx  = settingsHitTest(S, hx, hy, &part);
        if (idx >= 0) {
            mHeaderFocus  = -1;
            mSettingIndex = idx;
            auto& item    = mSettings.items[(std::size_t)idx];
            log("[ui] settings item " + std::to_string(idx) + " part=" +
                (part == SettingPart::Control ? "control" : "label"));
            // 顶层的分类按钮：点开进入下一层（不在这里改值）。
            if (item.kind == SettingKind::Category) {
                click();
                openSettingsCategory(idx);
                return true;
            }
            if (part == SettingPart::Control) {
                if (item.kind == SettingKind::Slider) {
                    // 选中即放大占满文本框，并按点击位置落到对应数值；之后可以继续拖。
                    click(); // Iris：按下滑块（抓住）就回一声
                    Rect track{};
                    for (auto const& box : S.slots) {
                        if (box.index == idx) {
                            track = box.control;
                            break;
                        }
                    }
                    if (track.valid()) {
                        float const t =
                            std::clamp((hx - track.x0) / std::max(track.x1 - track.x0, 1.f), 0.f, 1.f);
                        settingsSetSlider(mSettings, idx, t);
                        mSettingDragging = true;
                    }
                } else {
                    click(); // bool 取反 / enum 循环 / color 换色
                    settingsAdjust(mSettings, idx, 1); // bool 取反 / enum 循环 / color 换色
                }
            }
            clampSettingScroll();
            return true;
        }

        // 滚动条已经在上面统一处理（按住可拖），这里不用再判一次。
        return true;
    }

    // 行
    for (int i = 0; i < L.rowCount; ++i) {
        if (!L.rows[i].contains(hx, hy)) continue;

        // 列表页有"总开关 + 滚动"的偏移，行号要换算成条目下标。
        int const item = listIndexAtRow(i);
        if (item >= 0) {
            click();
            activateRow(item);
        }
        break;
    }
    return true;
}

} // namespace renderdragon_shader_loader::ui
