#include "ui/UiScreen.h"

#include "core/Hotkey.h"
#include "core/MaterialValidation.h"
#include "core/CompilerService.h"
#include "ui/ConfigurationName.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

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

// 列表里显示的名字：zip 包显示**完整文件名**（ExamplePack.zip、test.zip），和 Iris 一致；
// 目录形式的包就显示目录名。内部用的 pack.name（去扩展名）保持不变——它决定
// generated/<名字>/ 和 <名字>.zip.txt 这些路径，不能跟着显示名改。
std::string packLabel(ShaderPack const& pack) {
    if (!pack.archivePath.empty()) return pack.archivePath.filename().string();
    return pack.name;
}

std::string formatNumber(double v, bool integer) {
    if (integer) return std::to_string(std::llround(v));
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

std::vector<std::string> wrapTextForWidth(
    Canvas&                    canvas,
    std::string const&         text,
    float                      maxWidth,
    std::size_t                maxLines,
    float                      size = 1.f
) {
    std::vector<std::string> lines;
    if (text.empty() || maxWidth <= 0.f || maxLines == 0) return lines;

    std::string line;
    std::size_t pos = 0;
    while (pos < text.size() && lines.size() < maxLines) {
        std::size_t next = pos + 1;
        while (next < text.size()
               && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) {
            ++next;
        }
        std::string glyph = text.substr(pos, next - pos);
        pos               = next;

        std::string candidate = line + glyph;
        if (!line.empty() && canvas.textWidth(candidate, size) > maxWidth) {
            lines.push_back(line);
            line.clear();
            if (glyph == " ") {
                if (lines.size() >= maxLines) break;
                continue;
            }
        }
        line += glyph;
    }
    if (!line.empty() && lines.size() < maxLines) lines.push_back(line);
    if (pos < text.size() && lines.size() >= maxLines) {
        if (lines.empty()) lines.push_back("...");
        else lines.back() += "...";
    }
    return lines;
}

void popUtf8(std::string& text) {
    if (text.empty()) return;
    std::size_t pos = text.size() - 1;
    while (pos > 0 && ((static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80)) --pos;
    text.erase(pos);
}

std::string clipboardUtf8Text() {
#ifdef _WIN32
    if (!OpenClipboard(nullptr)) return {};
    std::string result;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr) {
        if (auto const* text = static_cast<wchar_t const*>(GlobalLock(data)); text != nullptr) {
            int const bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                std::string buffer((std::size_t)bytes, '\0');
                WideCharToMultiByte(
                    CP_UTF8,
                    0,
                    text,
                    -1,
                    buffer.data(),
                    bytes,
                    nullptr,
                    nullptr
                );
                result.assign(buffer.data(), (std::size_t)bytes - 1);
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return result;
#else
    return {};
#endif
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

bool Screen::blocked() const { return mHost.compiling && mHost.compiling(); }

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
    mFocus       = Focus::Content;
    mFocusIndex  = 0;
    mKeyCapture  = false;
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

// 宿主启动时按配置自动恢复了上次的光影，要把这份"已生效"的状态同步进界面，
// 否则面板会显示成"光影：已禁用"、玩家再点一次又会重复加载。
void Screen::setAppliedShaderPack(std::string const& packName, bool enabled) {
    auto const& packs = ShaderPackStore::getInstance().packs();
    mEnabled          = enabled;
    mEnabledApplied   = enabled;
    mAppliedPack      = -1;
    if (!packName.empty()) {
        for (std::size_t i = 0; i < packs.size(); ++i) {
            if (packs[i].name == packName) {
                mAppliedPack = (int)i;
                break;
            }
        }
    }
    if (mAppliedPack >= 0) {
        mPack = mAppliedPack;
        if (mMode == Mode::List) mRow = mPack + 1;
    }
    snapshotParameters();
}

void Screen::appendConfigurationNameText(std::string_view utf8Text) {
    if (!mPresetRenameActive || utf8Text.empty()) return;
    std::string combined = mPresetRenameText;
    combined.append(utf8Text);
    mPresetRenameText = normalizeConfigurationNameInput(combined);
}

void Screen::close() {
    mVisible = false;
    mPending = Pending::None;
    // 关面板时一定要退出"录键位"状态：否则宿主的热键监听会一直以为界面在等按键，
    // 面板热键就再也打不开面板了（只能靠 /shader ui）。
    mKeyCapture = false;
    if (mHost.closeScreen) mHost.closeScreen();
}

void Screen::dismiss() {
    // 游戏已经自行弹掉了占位屏幕；这里只回滚暂存状态，绝不在 onScreenExit
    // / 渲染回调里做重扫目录或解压这类文件系统工作。
    discardPending(false);
    mVisible = false;
    mPending = Pending::None;
    mKeyCapture = false;
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

// 状态行：优先显示界面自己的提示，没有才回落到宿主（编译服务）的状态。
std::string Screen::effectiveStatus() const {
    // 编译进行中一律显示宿主（编译服务）的实时进度；编译结束后，
    // 宿主会写"编译完成，新着色器已生效"这类结果，这条比界面的旧提示更权威。
    bool const compiling = mHost.compiling ? mHost.compiling() : false;
    if (compiling || (mHost.restartPending && mHost.restartPending())) {
        return mHost.statusMessage ? mHost.statusMessage() : std::string{};
    }
    if (!mNotice.empty()) return mNotice;
    return mHost.statusMessage ? mHost.statusMessage() : std::string{};
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

// 文字变了就重新开始计时（宿主那边改了编译状态也会走这里）。
void Screen::trackStatus(std::string const& message) {
    if (message == mStatusShown) return;
    mStatusShown   = message;
    mStatusShownAt = std::chrono::steady_clock::now();
}

void Screen::drawStatusLine(Canvas& canvas, float width, float y) const {
    std::string const message = effectiveStatus();
    float const       alpha   = statusAlpha(message);
    if (message.empty() || alpha <= 0.02f) return;
    bool const failed = mNotice.empty() ? (mHost.statusFailed && mHost.statusFailed()) : mNoticeFailed;
    Color      color  = failed ? failColor : accentColor;
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
    bool focused = bottomFocused(index); // 键盘焦点
    bool active  = hovered || focused;

    float width  = r.x1 - r.x0;
    float height = r.y1 - r.y0;

    // 常态：内部完全透明，只有一个灰框、白色文字；悬停/选中：边框变白，内部淡淡填一层白。
    // 文字**始终白色**（按需求：选中也不转黑），所以填充压得淡一点，保证白字还看得清。
    if (active) canvas.fill(r.x0, r.y0, width, height, buttonFillActive, 0.26f);
    strokeRect(canvas, r, active ? buttonBorderActive : buttonBorder, 0.9f);

    canvas.text(r.x0, textTopIn(r.y0, height), width, value, textColor, Align::Center, 1.f);
}

void Screen::drawHoverTooltip(Canvas& canvas, std::string const& text) {
    if (text.empty() || mMouseX < 0.f || mMouseY < 0.f) return;

    canvas.flush();
    float const w = std::min(canvas.textWidth(text, 1.f) + 10.f, 220.f);
    float const h = 16.f;
    float       x = mMouseX + 6.f;
    float       y = mMouseY - h - 4.f;
    x = std::clamp(x, 1.f, std::max(mLastWidth - w - 1.f, 1.f));
    y = std::clamp(y, 1.f, std::max(mLastHeight - h - 1.f, 1.f));

    canvas.fill(x, y, w, h, Color{0.f, 0.f, 0.f, 1.f}, 0.87f);
    strokeRect(canvas, Rect{x, y, x + w, y + h}, Color{0.87f, 0.87f, 0.87f, 1.f}, 0.95f);
    canvas.text(x + 4.f, textTopIn(y, h), w - 8.f, text, textColor, Align::Center, 1.f);
}

void Screen::drawPreviewButton(Canvas& canvas) {
    Rect const& r = mLayout.previewButton;
    if (!r.valid()) return;

    bool const hovered = r.contains(mMouseX, mMouseY);
    bool const focused = (mFocus == Focus::Preview && !mTransparentPreview);
    float const w      = r.x1 - r.x0;
    float const h      = r.y1 - r.y0;

    // 和底栏按钮同风格：常态内部透明、灰框白图标；悬停时白框 + 淡白填充。
    if (hovered || focused) canvas.fill(r.x0, r.y0, w, h, buttonFillActive, 0.26f);
    strokeRect(canvas, r, (hovered || focused) ? buttonBorderActive : buttonBorder, 0.9f);

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
    if (hovered) drawHoverTooltip(canvas, tr(mTransparentPreview ? "Panel" : "Transparent"));
}

void Screen::drawCompatibilityButton(Canvas& canvas) {
    Rect const& r = mLayout.compatibilityButton;
    if (!r.valid()) return;

    bool const hovered = r.contains(mMouseX, mMouseY);
    bool const focused = (mFocus == Focus::CompatibilityButton);
    float const w      = r.x1 - r.x0;
    float const h      = r.y1 - r.y0;
    Color const icon   = (hovered || focused) ? buttonBorderActive : buttonBorder;

    if (hovered || focused) canvas.fill(r.x0, r.y0, w, h, buttonFillActive, 0.26f);
    strokeRect(canvas, r, (hovered || focused) ? buttonBorderActive : buttonBorder, 0.9f);

    canvas.text(r.x0, textTopIn(r.y0, h), w, "i", icon, Align::Center, 1.f);

    if (hovered) drawHoverTooltip(canvas, tr("Compatibility check"));
}

// 左下角「设置」按钮：底栏左侧的模组设置入口（面板热键 / 强制灵动视效 / 自动加载）。
// 图标用色块拼一个齿轮：外圈四颗齿 + 中间镂空，不依赖字体里的符号。
void Screen::drawSettingsButton(Canvas& canvas) {
    Rect const& r = mLayout.settingsButton;
    if (!r.valid()) return;

    bool const  hovered = r.contains(mMouseX, mMouseY);
    bool const  focused = (mFocus == Focus::SettingsButton);
    float const w       = r.x1 - r.x0;
    float const h       = r.y1 - r.y0;
    Color const icon    = (hovered || focused) ? buttonBorderActive : buttonBorder;

    if (hovered || focused) canvas.fill(r.x0, r.y0, w, h, buttonFillActive, 0.26f);
    strokeRect(canvas, r, (hovered || focused) ? buttonBorderActive : buttonBorder, 0.9f);

    float const cx = (r.x0 + r.x1) / 2.f;
    float const cy = (r.y0 + r.y1) / 2.f;
    // 环（8 个 3x3 的小方块围成） + 上下左右四颗齿。
    for (float dx : {-6.f, 0.f, 6.f}) {
        for (float dy : {-6.f, 0.f, 6.f}) {
            if (dx == 0.f && dy == 0.f) continue;
            canvas.fill(cx + dx - 1.5f, cy + dy - 1.5f, 3.f, 3.f, icon, 1.f);
        }
    }
    canvas.fill(cx - 1.f, cy - 8.5f, 2.f, 3.f, icon, 1.f);
    canvas.fill(cx - 1.f, cy + 5.5f, 2.f, 3.f, icon, 1.f);
    canvas.fill(cx - 8.5f, cy - 1.f, 3.f, 2.f, icon, 1.f);
    canvas.fill(cx + 5.5f, cy - 1.f, 3.f, 2.f, icon, 1.f);
    if (hovered) drawHoverTooltip(canvas, tr("Settings"));
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

void Screen::drawCompatibility(Canvas& canvas, float width, float height) {
    drawContentBackdrop(canvas, width);
    drawHeader(canvas, width, headerTextY, mCompatibilityPack, tr("Compatibility"));

    auto const& L = mLayout;
    float const x0 = L.centerX - L.contentWidth / 2.f;
    float const x1 = L.centerX + L.contentWidth / 2.f;
    float       y  = L.contentTop + 8.f;
    float const contentBottom = L.buttons[0].y0 - 32.f;
    bool const  compact     = height < 260.f;
    float const rowTextSize = compact ? 1.f : 1.1f;
    float const rowHeight   = compact ? 16.f : 18.f;

    canvas.text(
        x0,
        y,
        (x1 - x0) * 0.5f,
        tr("Compiled materials") + ": " + std::to_string(mCompatibility.materialCount),
        textColor,
        Align::Left,
        rowTextSize
    );
    canvas.text(
        L.centerX,
        y,
        (x1 - x0) * 0.5f,
        tr("Render mode") + ": " + mCompatibility.renderMode,
        textColor,
        Align::Right,
        rowTextSize
    );
    y += rowHeight;

    std::string status = tr("Not compiled");
    Color       statusColor = textColor;
    if (mCompatibility.compiled) {
        if (!mCompatibility.nativeIO.available) {
            status = tr("Validation unavailable");
        } else if (mCompatibility.ok) {
            status = mCompatibility.nativeIO.warnings > 0
                       ? tr("Compatible with warnings")
                       : tr("Compatible");
        } else {
            status = tr("Compatibility failed");
        }
        statusColor = mCompatibility.ok ? accentColor : failColor;
    }
    canvas.text(x0, y, (x1 - x0) * 0.5f, tr("Material IO") + ": " + status, statusColor, Align::Left, rowTextSize);

    std::string overrideSummary = tr("None");
    if (mCompatibility.nativeIO.available && mCompatibility.nativeIO.overridesApplied > 0) {
        overrideSummary = std::to_string(mCompatibility.nativeIO.overridesApplied);
    } else if (mCompatibility.overridesFound) {
        overrideSummary = tr("Found");
    }
    canvas.text(
        L.centerX,
        y,
        (x1 - x0) * 0.5f,
        tr("IO overrides") + ": " + overrideSummary,
        mCompatibility.nativeIO.overriddenIssues.empty() ? textColor : selectColor,
        Align::Right,
        rowTextSize
    );
    y += rowHeight + 5.f;

    auto countFor = [&](CompatibilityIssueFilter filter) -> std::size_t {
        if (filter == CompatibilityIssueFilter::Mappings) {
            return mCompatibility.nativeIO.appliedMappings.size();
        }
        if (filter == CompatibilityIssueFilter::Overridden) {
            return mCompatibility.nativeIO.overriddenIssues.size();
        }
        std::size_t count = 0;
        for (auto const& item : mCompatibility.nativeIO.issues) {
            if (filter == CompatibilityIssueFilter::Errors && item.kind == "error") ++count;
            if (filter == CompatibilityIssueFilter::Warnings && item.kind == "warning") ++count;
        }
        return count;
    };

    std::string const tabLabels[4] = {
        tr("Errors") + " " + std::to_string(countFor(CompatibilityIssueFilter::Errors)),
        tr("Warnings") + " " + std::to_string(countFor(CompatibilityIssueFilter::Warnings)),
        tr("Overridden") + " " + std::to_string(countFor(CompatibilityIssueFilter::Overridden)),
        tr("Mappings") + " " + std::to_string(countFor(CompatibilityIssueFilter::Mappings)),
    };
    float const tabGap  = 4.f;
    float const tabTop  = y;
    float const tabWidth = (x1 - x0 - tabGap * 3.f) / 4.f;
    float const tabHeight = 18.f;
    for (int i = 0; i < 4; ++i) {
        mCompatibilityTabs[i] = {
            x0 + (tabWidth + tabGap) * (float)i,
            tabTop,
            x0 + (tabWidth + tabGap) * (float)i + tabWidth,
            tabTop + tabHeight,
        };
        bool const selected = (int)mCompatibilityFilter == i;
        bool const hovered  = mCompatibilityTabs[i].contains(mMouseX, mMouseY);
        if (selected || hovered) {
            canvas.fill(
                mCompatibilityTabs[i].x0,
                mCompatibilityTabs[i].y0,
                tabWidth,
                tabHeight,
                selected ? buttonFillActive : panelColor,
                selected ? 0.24f : 0.4f
            );
        }
        strokeRect(
            canvas,
            mCompatibilityTabs[i],
            selected ? buttonBorderActive : buttonBorder,
            selected ? 1.f : 0.75f
        );
        canvas.text(
            mCompatibilityTabs[i].x0,
            textTopIn(mCompatibilityTabs[i].y0, tabHeight),
            tabWidth,
            tabLabels[i],
            selected ? textColor : dimColor,
            Align::Center,
            1.f
        );
    }
    y += tabHeight + 6.f;

    mCompatibilityViewport = {x0, y, x1 - 7.f, contentBottom};
    mCompatibilityScrollTrack = {};
    mCompatibilityScrollHandle = {};
    mCompatibilityMaxScroll = 0.f;

    std::string detail;
    if (!mCompatibility.compiled) {
        detail = tr("This pack has no compiled material output yet.");
    } else if (!mCompatibility.nativeIO.available) {
        detail = mCompatibility.detail.empty()
                   ? tr("Native IO validator did not return a structured report.")
                   : mCompatibility.detail;
    }

    if (!detail.empty()) {
        auto const lines = wrapTextForWidth(
            canvas,
            detail,
            mCompatibilityViewport.x1 - mCompatibilityViewport.x0 - 8.f,
            6
        );
        float textY = mCompatibilityViewport.y0 + 3.f;
        for (auto const& line : lines) {
            if (textY + 12.f > mCompatibilityViewport.y1) break;
            canvas.text(
                x0,
                textY,
                x1 - x0,
                line,
                mCompatibility.ok ? textColor : failColor,
                Align::Center,
                1.f
            );
            textY += 14.f;
        }
    } else {
        struct DisplayLine {
            std::string text;
            Color       color;
        };
        struct DisplayIssue {
            std::vector<DisplayLine> lines;
            float                    height = 0.f;
        };

        std::vector<DisplayIssue> display;
        float const textWidthLimit = mCompatibilityViewport.x1 - mCompatibilityViewport.x0 - 10.f;
        if (mCompatibilityFilter == CompatibilityIssueFilter::Mappings) {
            for (auto const& item : mCompatibility.nativeIO.appliedMappings) {
                DisplayIssue entry;
                std::string headerText =
                    "MAPPING [" + item.section + "] " + item.id;
                auto header = wrapTextForWidth(canvas, headerText, textWidthLimit, 2);
                for (auto& line : header) entry.lines.push_back({std::move(line), accentColor});

                std::string target = item.material;
                if (!item.pass.empty()) target += "/" + item.pass;
                std::string bodyText = target + ": " + item.source + " -> " + item.target;
                auto body = wrapTextForWidth(canvas, bodyText, textWidthLimit, 3);
                for (auto& line : body) entry.lines.push_back({std::move(line), textColor});

                if (!item.reason.empty()) {
                    auto reason = wrapTextForWidth(
                        canvas,
                        tr("Reason") + ": " + item.reason,
                        textWidthLimit,
                        2
                    );
                    for (auto& line : reason) entry.lines.push_back({std::move(line), selectColor});
                }
                entry.height = (float)entry.lines.size() * 12.f + 7.f;
                display.push_back(std::move(entry));
            }
        } else {
            auto const& source = compatibilityIssues();
            for (auto const& item : source) {
                bool const wanted =
                    mCompatibilityFilter == CompatibilityIssueFilter::Overridden
                    || (mCompatibilityFilter == CompatibilityIssueFilter::Errors && item.kind == "error")
                    || (mCompatibilityFilter == CompatibilityIssueFilter::Warnings && item.kind == "warning");
                if (!wanted) continue;

                DisplayIssue entry;
                std::string target = item.material;
                if (!item.pass.empty()) target += "/" + item.pass;
                if (!item.buffer.empty()) target += "/" + item.buffer;
                std::string kind = item.kind;
                std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                if (mCompatibilityFilter == CompatibilityIssueFilter::Overridden) kind = "OVERRIDDEN";
                Color const kindColor = mCompatibilityFilter == CompatibilityIssueFilter::Overridden
                                          ? selectColor
                                          : (item.kind == "warning" ? selectColor : failColor);

                std::string headerText = kind;
                headerText += " [";
                headerText += item.code;
                headerText += "] ";
                headerText += target;
                auto header = wrapTextForWidth(canvas, headerText, textWidthLimit, 2);
                for (auto& line : header) entry.lines.push_back({std::move(line), kindColor});

                auto body = wrapTextForWidth(canvas, item.message, textWidthLimit, 3);
                for (auto& line : body) entry.lines.push_back({std::move(line), textColor});

                if (!item.overrideId.empty() || !item.reason.empty()) {
                    std::string overrideText = tr("Override") + " " + item.overrideId;
                    if (!item.reason.empty()) overrideText += ": " + item.reason;
                    auto reason = wrapTextForWidth(canvas, overrideText, textWidthLimit, 3);
                    for (auto& line : reason) entry.lines.push_back({std::move(line), selectColor});
                }
                entry.height = (float)entry.lines.size() * 12.f + 7.f;
                display.push_back(std::move(entry));
            }
        }

        float totalHeight = 0.f;
        for (auto const& item : display) totalHeight += item.height;
        float const viewportHeight = mCompatibilityViewport.y1 - mCompatibilityViewport.y0;
        mCompatibilityMaxScroll = std::max(0.f, totalHeight - viewportHeight);
        mCompatibilityScroll    = std::clamp(mCompatibilityScroll, 0.f, mCompatibilityMaxScroll);

        if (display.empty()) {
            std::string emptyKey = "No error issues";
            if (mCompatibilityFilter == CompatibilityIssueFilter::Warnings) emptyKey = "No warning issues";
            if (mCompatibilityFilter == CompatibilityIssueFilter::Overridden) emptyKey = "No overridden issues";
            if (mCompatibilityFilter == CompatibilityIssueFilter::Mappings) emptyKey = "No applied mappings";
            canvas.text(
                x0,
                mCompatibilityViewport.y0 + 4.f,
                x1 - x0,
                tr(emptyKey),
                dimColor,
                Align::Center,
                1.f
            );
        } else {
            float itemTop = mCompatibilityViewport.y0 - mCompatibilityScroll;
            for (auto const& item : display) {
                float lineY = itemTop;
                for (auto const& line : item.lines) {
                    if (lineY >= mCompatibilityViewport.y0 - 1.f
                        && lineY + 12.f <= mCompatibilityViewport.y1 + 1.f) {
                        canvas.text(x0, lineY, x1 - x0, line.text, line.color, Align::Left, 1.f);
                    }
                    lineY += 12.f;
                }
                itemTop += item.height;
                if (itemTop > mCompatibilityViewport.y1) break;
            }
        }

        if (mCompatibilityMaxScroll > 0.f && viewportHeight > 0.f) {
            float const trackWidth = 3.f;
            mCompatibilityScrollTrack = {
                x1 - trackWidth,
                mCompatibilityViewport.y0,
                x1,
                mCompatibilityViewport.y1,
            };
            float const handleHeight = std::max(
                12.f,
                viewportHeight * viewportHeight / std::max(totalHeight, viewportHeight)
            );
            float const handleTop = mCompatibilityScrollTrack.y0
                + (viewportHeight - handleHeight) * (mCompatibilityScroll / mCompatibilityMaxScroll);
            mCompatibilityScrollHandle = {
                mCompatibilityScrollTrack.x0,
                handleTop,
                mCompatibilityScrollTrack.x1,
                handleTop + handleHeight,
            };
            drawScrollBar(canvas, mCompatibilityScrollTrack, mCompatibilityScrollHandle);
        }
    }

    drawStatusLine(canvas, width, L.buttons[0].y0 - 20.f);
    canvas.fill(0.f, L.buttons[0].y0 - 8.f, width, 1.f, barColor, 0.85f);
    drawButton(canvas, L.buttons[2], tr("Back"), 2);
    drawButton(
        canvas,
        L.buttons[3],
        mCompatibility.nativeIO.json.empty() ? tr("No report") : tr("Export report"),
        3
    );
}

// 设置页（Iris 风格）：顶栏标题 + 右上角 导入/导出/重置 + 左上角返回，
// 中间是可滚动的参数列表（两列 / 整行两种版式，滑块选中后放大占满文本框）。
void Screen::drawSettings(Canvas& canvas, float width, float height) {
    drawContentBackdrop(canvas, width);

    // 顶栏：和列表页同一套观感（两行居中标题 + 底下一条贯穿分界线）。
    canvas.fill(0.f, 0.f, width, chromeHeaderLineY, panelColor, chromeAlpha);
    float const titleY = width < 420.f ? 1.f : headerTextY;
    canvas.text(0.f, titleY, width, mSettings.title, textColor, Align::Center, 1.f);
    if (width >= 420.f) {
        canvas.text(0.f, headerTextY + 14.f, width, tr(mSettings.subtitle), dimColor, Align::Center, 1.f);
    }
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

    smallButton(S.headerBack, "< " + tr("Back"), headerFocused(0) || S.headerBack.contains(mMouseX, mMouseY));
    if (mMode == Mode::Settings) {
        std::string const headerKeys[4] = {"Save", "Import", "Export", "Reset"};
        for (int i = 0; i < S.headerCount; ++i) {
            bool const active = headerFocused(i + 1) || S.header[i].contains(mMouseX, mMouseY);
            smallButton(S.header[i], tr(headerKeys[i]), active);
        }
    }

    if (mMode == Mode::Settings && mPresetBrowserOpen) {
        drawPresetBrowser(canvas, S);

        {
            std::string const message = effectiveStatus();
            float const       alpha   = statusAlpha(message);
            if (!message.empty() && alpha > 0.02f) {
                bool const failed = mNotice.empty() ? (mHost.statusFailed && mHost.statusFailed()) : mNoticeFailed;
                Color      color  = failed ? failColor : accentColor;
                color.a           = alpha;
                Rect const bar{S.viewport.x0, S.viewport.y1 - 15.f, S.viewport.x1, S.viewport.y1};
                canvas.text(bar.x0, textTopIn(bar.y0, 15.f), bar.x1 - bar.x0, message, color, Align::Center, 1.f);
            }
        }

        canvas.fill(0.f, chromeBottomBarTop(height), width, 1.f, barColor, 0.85f);
        std::string const texts[5] = {
            tr("Open configuration folder"),
            tr("Rename configuration"),
            tr("Delete configuration"),
            tr("Apply"),
            tr("Done"),
        };
        for (int i = 0; i < mLayout.buttonCount; ++i) drawButton(canvas, mLayout.buttons[i], texts[i], i);
        return;
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
        case SettingKind::KeyBind: {
            // 热键行：值框里显示键名；正在等玩家按键时显示提示。
            strokeRect(canvas, inner, active ? buttonBorderActive : innerBorderColor, 0.95f);
            std::string text = item.value.empty() ? tr("NONE") : item.value;
            if (mKeyCapture && box.index == mSettingIndex) text = tr("Press a key...");
            canvas.text(
                inner.x0,
                textTopIn(inner.y0, innerH),
                innerW,
                text,
                (mKeyCapture && box.index == mSettingIndex) ? selectColor : valueColor,
                Align::Center,
                1.f
            );
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
            bool const failed = mNotice.empty() ? (mHost.statusFailed && mHost.statusFailed()) : mNoticeFailed;
            Color      color  = failed ? failColor : accentColor;
            color.a           = alpha;
            Rect const bar{S.viewport.x0, S.viewport.y1 - 15.f, S.viewport.x1, S.viewport.y1};
            canvas.text(bar.x0, textTopIn(bar.y0, 15.f), bar.x1 - bar.x0, message, color, Align::Center, 1.f);
        }
    }
    drawPresetBar(canvas, S);
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

void Screen::drawPresetBar(Canvas& canvas, SettingsLayout const& layout) {
    if (mMode != Mode::Settings || !layout.configBar.valid()) return;

    auto const& packs   = ShaderPackStore::getInstance().packs();
    bool const  hasPack = (mPack >= 0 && (std::size_t)mPack < packs.size());
    if (!hasPack) return;

    auto const& pack = packs[(std::size_t)mPack];
    std::string current = pack.currentPreset.empty() ? "default" : pack.currentPreset;
    for (auto const& preset : pack.presets) {
        if (preset.id == pack.currentPreset) {
            current = preset.label.empty() ? preset.id : preset.label;
            break;
        }
    }
    if (settingsDirty()) current = tr("Custom");

    float const barW = layout.configBar.x1 - layout.configBar.x0;
    float const barH = layout.configBar.y1 - layout.configBar.y0;
    canvas.fill(layout.configBar.x0, layout.configBar.y0, barW, barH, Color{0.f, 0.f, 0.f, 1.f}, 0.35f);
    strokeRect(canvas, layout.configButton, buttonBorder, 0.85f);
    canvas.text(
        layout.configButton.x0 + 4.f,
        textTopIn(layout.configButton.y0, barH),
        layout.configButton.x1 - layout.configButton.x0 - 8.f,
        tr("Configuration") + ": " + current,
        textColor,
        Align::Center,
        1.f
    );
}

int Screen::presetBrowserVisibleRows() const {
    float const top    = chromeHeaderLineY + 6.f;
    float const bottom = chromeBottomBarTop(mLastHeight) - 6.f;
    constexpr float rowH   = 20.f;
    constexpr float rowGap = 4.f;
    return std::max(1, (int)std::floor((bottom - top + rowGap) / (rowH + rowGap)));
}

void Screen::ensurePresetBrowserVisible() {
    int const visibleRows = presetBrowserVisibleRows();
    int const selectedRow = mPresetBrowserIndex / 2;
    if (selectedRow < mPresetBrowserFirst) mPresetBrowserFirst = selectedRow;
    else if (selectedRow >= mPresetBrowserFirst + visibleRows) {
        mPresetBrowserFirst = selectedRow - visibleRows + 1;
    }
    mPresetBrowserFirst = std::max(mPresetBrowserFirst, 0);
}

void Screen::drawPresetBrowser(Canvas& canvas, SettingsLayout const& layout) {
    mPresetBrowserRects.clear();
    mPresetBrowserTrack  = {};
    mPresetBrowserHandle = {};

    auto const& packs   = ShaderPackStore::getInstance().packs();
    bool const  hasPack = (mPack >= 0 && (std::size_t)mPack < packs.size());
    if (!hasPack) return;
    auto const& pack = packs[(std::size_t)mPack];

    Rect const& content = layout.content;
    constexpr float topPad   = 6.f;
    constexpr float colGap   = 4.f;
    constexpr float rowH     = 20.f;
    constexpr float rowGap   = 4.f;
    float const left  = content.x0;
    float const right = content.x1;
    float const top    = chromeHeaderLineY + topPad;
    float const bottom = chromeBottomBarTop(mLastHeight) - 6.f;
    float const cellW  = std::floor((right - left - colGap) / 2.f);
    int const visibleRows = presetBrowserVisibleRows();

    for (int row = 0; row < visibleRows; ++row) {
        int const firstIndex = (mPresetBrowserFirst + row) * 2;
        for (int col = 0; col < 2; ++col) {
            int const index = firstIndex + col;
            if (index < 0 || (std::size_t)index >= pack.presets.size()) continue;
            auto const& preset = pack.presets[(std::size_t)index];
            float const x0 = left + (float)col * (cellW + colGap);
            float const y0 = top + (float)row * (rowH + rowGap);
            Rect const  button{x0, y0, x0 + cellW, y0 + rowH};
            bool const  hovered = button.contains(mMouseX, mMouseY);
            bool const  selected = index == mPresetBrowserIndex;
            canvas.fill(
                button.x0,
                button.y0,
                button.x1 - button.x0,
                button.y1 - button.y0,
                Color{0.f, 0.f, 0.f, 1.f},
                hovered ? 0.72f : 0.45f
            );
            strokeRect(
                canvas,
                button,
                (hovered || selected) ? buttonBorderActive : buttonBorder,
                (hovered || selected) ? 0.95f : 0.82f
            );
            std::string label = preset.label.empty() ? preset.id : preset.label;
            if (preset.compiled) label += "  " + std::to_string(preset.materialCount);
            canvas.text(
                button.x0 + 5.f,
                textTopIn(button.y0, rowH),
                button.x1 - button.x0 - 10.f,
                label,
                textColor,
                Align::Left,
                1.f
            );
            mPresetBrowserRects.push_back(button);
        }
    }

    int const totalRows = ((int)pack.presets.size() + 1) / 2;
    if (totalRows > visibleRows) {
        float const trackX = right + 3.f;
        mPresetBrowserTrack = {trackX, top, trackX + 3.f, bottom};
        float const trackH  = std::max(bottom - top, 1.f);
        float const handleH = std::max(18.f, trackH * (float)visibleRows / (float)totalRows);
        float const maxFirst = std::max(totalRows - visibleRows, 0);
        float const t = maxFirst > 0 ? (float)mPresetBrowserFirst / (float)maxFirst : 0.f;
        float const hy = top + (trackH - handleH) * std::clamp(t, 0.f, 1.f);
        mPresetBrowserHandle = {trackX, hy, trackX + 3.f, hy + handleH};
        drawScrollBar(canvas, mPresetBrowserTrack, mPresetBrowserHandle);
    }

    if (mPresetRenameActive) {
        float const boxW = std::min(right - left - 20.f, 320.f);
        float const boxH = 68.f;
        float const boxX = (left + right - boxW) / 2.f;
        float const boxY = (top + bottom - boxH) / 2.f;
        canvas.fill(boxX, boxY, boxW, boxH, panelColor, 0.62f);
        strokeRect(canvas, Rect{boxX, boxY, boxX + boxW, boxY + boxH}, buttonBorderActive, 1.f);
        canvas.text(boxX, boxY + 6.f, boxW, tr("Rename configuration"), textColor, Align::Center, 1.f);

        Rect const input{boxX + 10.f, boxY + 24.f, boxX + boxW - 10.f, boxY + 44.f};
        canvas.fill(input.x0, input.y0, input.x1 - input.x0, input.y1 - input.y0, Color{0.f, 0.f, 0.f, 1.f}, 0.45f);
        strokeRect(canvas, input, innerBorderColor, 0.95f);
        canvas.text(
            input.x0 + 4.f,
            textTopIn(input.y0, input.y1 - input.y0),
            input.x1 - input.x0 - 8.f,
            mPresetRenameText,
            textColor,
            Align::Left,
            1.f
        );
        canvas.text(
            boxX,
            boxY + 49.f,
            boxW,
            tr("Type a name, Enter to confirm, Esc to cancel"),
            dimColor,
            Align::Center,
            1.f
        );
    }
}

void Screen::selectPresetByIndex(int index) {
    auto const& packs   = ShaderPackStore::getInstance().packs();
    bool const  hasPack = (mPack >= 0 && (std::size_t)mPack < packs.size());
    if (!hasPack) return;
    auto const& pack = packs[(std::size_t)mPack];
    if (index < 0 || (std::size_t)index >= pack.presets.size()) return;

    // Switching configurations should not discard edits already made in the current one.
    // Persist the current page values into the active settings file before activation.
    if (settingsDirty()) {
        auto& store = ShaderPackStore::getInstance();
        for (auto const& item : mSettings.items) {
            if (item.kind == SettingKind::Category) continue;
            store.setParameterValueByName(pack.name, item.key, item.value);
        }
        ShaderPackStore::saveSettings(pack);
    }

    auto const& preset = pack.presets[(std::size_t)index];
    if (!ShaderPackStore::getInstance().selectPreset(pack.name, preset.id)) return;
    mPresetBrowserOpen  = false;
    mPresetRenameActive = false;
    mPresetBrowserIndex = index;

    auto const& refreshed = ShaderPackStore::getInstance().packs()[(std::size_t)mPack];
    mSettings = settingsFromPack(refreshed, mHost.locale ? mHost.locale() : std::string{});
    mSettings.title = packLabel(refreshed);
    mSettings.subtitle = "Configure";
    mSettingsSnapshot = dumpSettings(mSettings);
    log("[ui] configuration selected: " + refreshed.currentPreset);
    notice(
        preset.compiled ? tr("Configuration selected") : tr("Configuration has no compiled output"),
        !preset.compiled
    );
}

bool Screen::handlePresetBrowserKey(int keyCode) {
    if (!mPresetBrowserOpen) return false;
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) return true;
    int const count = (int)packs[(std::size_t)mPack].presets.size();
    if (count == 0) return true;

    switch (keyCode) {
    case 0x1B: // Esc
        mPresetBrowserOpen = false;
        mPresetRenameActive = false;
        return true;
    case 0x25: // Left
        mPresetBrowserIndex = std::max(mPresetBrowserIndex - 1, 0);
        ensurePresetBrowserVisible();
        return true;
    case 0x27: // Right
        mPresetBrowserIndex = std::min(mPresetBrowserIndex + 1, count - 1);
        ensurePresetBrowserVisible();
        return true;
    case 0x26: // Up
        mPresetBrowserIndex = std::max(mPresetBrowserIndex - 2, 0);
        ensurePresetBrowserVisible();
        return true;
    case 0x28: // Down
        mPresetBrowserIndex = std::min(mPresetBrowserIndex + 2, count - 1);
        ensurePresetBrowserVisible();
        return true;
    case 0x0D: // Enter
        selectPresetByIndex(mPresetBrowserIndex);
        return true;
    case 0x09: // Tab: move focus to the management bar
        mFocus      = Focus::Bottom;
        mFocusIndex = 0;
        return true;
    default:
        return true;
    }
}

bool Screen::handlePresetBrowserBottomKey(int keyCode) {
    switch (keyCode) {
    case 0x1B:
    case 0x26:
        focusContent();
        return true;
    case 0x25:
        mFocusIndex = std::max(mFocusIndex - 1, 0);
        return true;
    case 0x27:
        mFocusIndex = std::min(mFocusIndex + 1, 4);
        return true;
    case 0x0D:
        activatePresetBrowserBottom(mFocusIndex);
        return true;
    default:
        return true;
    }
}

bool Screen::handlePresetRenameKey(int keyCode) {
#ifdef _WIN32
    if (keyCode == 0x56 && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        mPresetRenameText = normalizeConfigurationNameInput(
            mPresetRenameText + clipboardUtf8Text()
        );
        return true;
    }
#endif

    switch (keyCode) {
    case 0x1B:
        mPresetRenameActive = false;
        return true;
    case 0x0D: {
        auto const& packs = ShaderPackStore::getInstance().packs();
        if (mPack < 0 || (std::size_t)mPack >= packs.size()) {
            mPresetRenameActive = false;
            return true;
        }
        auto const& pack = packs[(std::size_t)mPack];
        if (mPresetBrowserIndex < 0 || (std::size_t)mPresetBrowserIndex >= pack.presets.size()) {
            mPresetRenameActive = false;
            return true;
        }

        std::string const oldId = pack.presets[(std::size_t)mPresetBrowserIndex].id;
        std::string const newId = mPresetRenameText;
        if (newId.empty()) {
            notice(tr("Configuration name cannot be empty"), true);
            return true;
        }
        if (newId == oldId) {
            mPresetRenameActive = false;
            return true;
        }
        if (!ShaderPackStore::getInstance().renameLocalPreset(pack.name, oldId, newId)) {
            notice(tr("Configuration could not be renamed"), true);
            return true;
        }

        auto const& refreshed = ShaderPackStore::getInstance().packs()[(std::size_t)mPack];
        for (int i = 0; i < (int)refreshed.presets.size(); ++i) {
            if (refreshed.presets[(std::size_t)i].id == newId) {
                mPresetBrowserIndex = i;
                break;
            }
        }
        mSettings = settingsFromPack(refreshed, mHost.locale ? mHost.locale() : std::string{});
        mSettings.title = packLabel(refreshed);
        mSettings.subtitle = "Configure";
        mSettingsSnapshot = dumpSettings(mSettings);
        mPresetRenameActive = false;
        notice(tr("Configuration renamed"), false);
        return true;
    }
    case 0x08:
        popUtf8(mPresetRenameText);
        return true;
    default:
        if (mPresetRenameText.size() >= kMaxConfigurationNameBytes) return true;
        if (keyCode >= 0x41 && keyCode <= 0x5A) {
            mPresetRenameText.push_back((char)std::tolower(keyCode));
        } else if (keyCode >= 0x30 && keyCode <= 0x39) {
            mPresetRenameText.push_back((char)keyCode);
        } else if (keyCode == 0xBD) {
            mPresetRenameText.push_back('-');
        }
        return true;
    }
}

void Screen::beginPresetRename() {
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) return;
    auto const& pack = packs[(std::size_t)mPack];
    if (mPresetBrowserIndex < 0 || (std::size_t)mPresetBrowserIndex >= pack.presets.size()) return;
    auto const& preset = pack.presets[(std::size_t)mPresetBrowserIndex];
    if (preset.origin != "local") {
        notice(tr("Author default configurations cannot be renamed"), true);
        return;
    }
    mPresetRenameText   = preset.id;
    mPresetRenameActive = true;
}

void Screen::deleteSelectedPreset() {
    auto& store = ShaderPackStore::getInstance();
    auto const& packs = store.packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) return;
    auto const& pack = packs[(std::size_t)mPack];
    if (mPresetBrowserIndex < 0 || (std::size_t)mPresetBrowserIndex >= pack.presets.size()) return;
    auto const& preset = pack.presets[(std::size_t)mPresetBrowserIndex];
    if (preset.origin != "local") {
        notice(tr("Author default configurations cannot be deleted"), true);
        return;
    }
    if (pack.presets.size() <= 1) {
        notice(tr("The last configuration cannot be deleted"), true);
        return;
    }
    if (!store.deleteLocalPreset(pack.name, preset.id)) {
        notice(tr("Configuration could not be deleted"), true);
        return;
    }

    auto const& refreshed = store.packs()[(std::size_t)mPack];
    mPresetBrowserIndex = std::clamp(mPresetBrowserIndex, 0, std::max((int)refreshed.presets.size() - 1, 0));
    mSettings = settingsFromPack(refreshed, mHost.locale ? mHost.locale() : std::string{});
    mSettings.title = packLabel(refreshed);
    mSettings.subtitle = "Configure";
    mSettingsSnapshot = dumpSettings(mSettings);
    snapshotParameters();
    notice(tr("Configuration deleted"), false);
}

void Screen::openSelectedPresetFolder() {
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) return;
    if (mHost.openConfigurationFolder) mHost.openConfigurationFolder(packs[(std::size_t)mPack]);
}

void Screen::activatePresetBrowserBottom(int index) {
    switch (index) {
    case 0:
        mPresetRenameActive = false;
        openSelectedPresetFolder();
        break;
    case 1: beginPresetRename(); break;
    case 2:
        mPresetRenameActive = false;
        deleteSelectedPreset();
        break;
    case 3:
        mPresetRenameActive = false;
        selectPresetByIndex(mPresetBrowserIndex);
        if (!mPresetBrowserOpen) focusContent();
        break;
    case 4:
        mPresetBrowserOpen = false;
        mPresetRenameActive = false;
        focusContent();
        break;
    default: break;
    }
}

void Screen::saveCurrentConfiguration() {
    auto& store = ShaderPackStore::getInstance();
    auto const& packs = store.packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) {
        notice(tr("Select a shader pack first"), true);
        return;
    }

    auto const& pack = packs[(std::size_t)mPack];
    for (auto const& item : mSettings.items) {
        if (item.kind == SettingKind::Category) continue;
        store.setParameterValueByName(pack.name, item.key, item.value);
    }
    ShaderPackStore::saveSettings(pack);

    std::string id;
    for (int suffix = 0;; ++suffix) {
        std::string candidate = suffix == 0 ? "config" : "config-" + std::to_string(suffix);
        bool exists = std::any_of(
            pack.presets.begin(),
            pack.presets.end(),
            [&](ShaderPack::Preset const& preset) { return preset.id == candidate; }
        );
        if (!exists) {
            id = std::move(candidate);
            break;
        }
    }

    if (!store.createLocalPreset(pack.name, id)) {
        notice(tr("Configuration could not be saved"), true);
        log("[ui] configuration save failed: " + pack.name);
        return;
    }

    auto const& refreshed = store.packs()[(std::size_t)mPack];
    mSettings = settingsFromPack(refreshed, mHost.locale ? mHost.locale() : std::string{});
    mSettings.title = packLabel(refreshed);
    mSettings.subtitle = "Configure";
    mSettingsSnapshot = dumpSettings(mSettings);
    for (int i = 0; i < (int)refreshed.presets.size(); ++i) {
        if (refreshed.presets[(std::size_t)i].id == id) {
            mPresetBrowserIndex = i;
            break;
        }
    }
    mPresetBrowserFirst = 0;
    notice(tr("Configuration saved"), false);
    log("[ui] configuration saved as '" + id + "'");
}

void Screen::resetCurrentConfiguration() {
    auto& store = ShaderPackStore::getInstance();
    auto const& packs = store.packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) {
        notice(tr("Reset failed"), true);
        return;
    }

    auto const packName = packs[(std::size_t)mPack].name;
    auto const presetId = packs[(std::size_t)mPack].currentPreset;
    if (presetId.empty() || !store.selectPreset(packName, presetId)) {
        notice(tr("Reset failed"), true);
        return;
    }

    auto const& refreshed = store.packs()[(std::size_t)mPack];
    mSettings = settingsFromPack(refreshed, mHost.locale ? mHost.locale() : std::string{});
    mSettings.title = packLabel(refreshed);
    mSettings.subtitle = "Configure";
    mSettingsSnapshot = dumpSettings(mSettings);
    snapshotParameters();
    for (int i = 0; i < (int)refreshed.presets.size(); ++i) {
        if (refreshed.presets[(std::size_t)i].id == refreshed.currentPreset) {
            mPresetBrowserIndex = i;
            break;
        }
    }
    notice(tr("Reset to current configuration"), false);
    log("[ui] configuration reset to '" + refreshed.currentPreset + "'");
}

void Screen::render(Canvas& canvas, float width, float height) {
    if (!mVisible) return;
    if (width <= 0.f || height <= 0.f) return;

    mLayout = buildLayout(width, height);
    mLastWidth  = width;
    mLastHeight = height;

    // 设置页版式：每帧重建。先用"没有悬停"的版式定位鼠标下的项（格子几何和悬停无关），
    // 再带上悬停项重建 —— 滑块只要鼠标移上去就展开、一移开立刻复原。
    if (mMode == Mode::Settings || mMode == Mode::Preferences) {
        // -2 / 模组设置页 = 整页参数行；-1 = 分类按钮网格；>= 0 = 某一层的参数行。
        SettingPageStyle const style = (mSettingsPage <= -2 || mMode == Mode::Preferences)
            ? SettingPageStyle::Rows
            : (mSettingsPage == -1 ? SettingPageStyle::ButtonGrid : SettingPageStyle::Rows);
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

    // 编译中：锁住一切操作，只显示进度。
    if (blocked()) {
        drawHeader(canvas, width, headerTextY, tr("Shader Packs"), tr("Compiling..."));
        canvas.text(0.f, height / 2.f - 7.f, width, tr("Compiling..."), selectColor, Align::Center, 1.f);
        canvas.text(
            0.f,
            height / 2.f + 9.f,
            width,
            mHost.statusMessage ? mHost.statusMessage() : std::string{},
            dimColor,
            Align::Center,
            1.f
        );
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
    case Mode::Preferences:
        drawSettings(canvas, width, height);
        break;
    case Mode::Compatibility:
        drawCompatibility(canvas, width, height);
        break;
    }

    if (mMode == Mode::List) drawCompatibilityButton(canvas);
    drawPreviewButton(canvas);
    drawSettingsButton(canvas);
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
//   开关/选包变了 → 真的加载（或还原原版）；参数变了 → 增量编译，成功后由宿主热重载。
// 返回 false = 这次没能应用（例如选中的包没有编译产物），调用方应留在界面上让玩家看到提示。
bool Screen::applyPending() {
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (!pendingChanged()) return true; // 没有任何改动：点「应用」什么都不做

    bool const paramsChanged = dirty();
    // 诊断：把"为什么判定成有改动"写进日志，避免以后再靠猜。
    if (paramsChanged && mPack >= 0 && (std::size_t)mPack < packs.size()) {
        auto const&         pack = packs[(std::size_t)mPack];
        std::string         current;
        for (auto const& p : pack.parameters) current += p.name + "=" + p.current + "\n";
        if (current != mSnapshot) {
            log(
                "[ui] parameters dirty: snapshot=" + std::to_string(mSnapshot.size())
                + " bytes, current=" + std::to_string(current.size()) + " bytes"
            );
        } else {
            log("[ui] parameters dirty but snapshot matches (state/selection changed)");
        }
    }
    bool       compileStarted           = false;
    bool       compileAppliedImmediately = false;
    if (paramsChanged && mEnabled && mPack >= 0 && (std::size_t)mPack < packs.size()) {
        auto const& pack = packs[(std::size_t)mPack];
        ShaderPackStore::saveSettings(pack);
        auto const result =
            mHost.compile ? mHost.compile(pack) : CompileRequestResult::Failed;
        if (result == CompileRequestResult::Failed) {
            notice(tr("Compile could not start"), true);
            return false;
        }
        compileStarted            = result == CompileRequestResult::Queued;
        compileAppliedImmediately = result == CompileRequestResult::AppliedImmediately;
    }

    bool const stateChanged = (mEnabled != mEnabledApplied) || (mEnabled && mPack != mAppliedPack);
    if (compileStarted) {
        // 编译是异步的。成功回调才会把新包标记为“已应用”，失败时必须保留
        // 原来的 applied 状态，不能提前显示为已启用或写入 lastShaderPack。
        log(
            "[ui] compile queued; applied state retained (pack=" + std::to_string(mPack)
            + ", applied=" + std::to_string(mAppliedPack) + ")"
        );
        snapshotParameters();
        notice(tr("Compiling..."), false);
        return true;
    }
    if (stateChanged) {
        if (!mEnabled) {
            if (mHost.restoreVanilla) mHost.restoreVanilla();
            mAppliedPack    = -1;
            mEnabledApplied = false;
            mNotice.clear();
            log("[ui] shaders disabled");
            // 记住"现在没开光影"：下次启动就不要再自动挂回去。
            if (mHost.rememberSelection) mHost.rememberSelection({}, false);
        } else {
            if (mPack < 0 || (std::size_t)mPack >= packs.size()) {
                notice(tr("Select a shader pack first"), true);
                return false;
            }
            auto const& pack = packs[(std::size_t)mPack];
            if (!compileAppliedImmediately) {
                std::string validationError;
                // 兼容层/编译格式变化后，旧产物即使能通过校验也要重编：
                // meta.json 的 compilerVersion 与当前版本不一致就视为过期。
                bool const stale = compiledOutputNeedsRebuild(pack.binsDir);
                if (stale || !validateCompiledOutput(pack.binsDir, &validationError)) {
                    if (stale) {
                        log("[ui] '" + pack.name + "' compiled output is stale (compiler version changed)");
                    }
                    auto const result =
                        mHost.compile ? mHost.compile(pack) : CompileRequestResult::Failed;
                    if (result == CompileRequestResult::Failed) {
                        log("[ui] '" + pack.name + "' output invalid and compile was refused: " + validationError);
                        notice(tr("Compile could not start"), true);
                        return false;
                    }
                    if (result == CompileRequestResult::Queued) {
                        log(
                            "[ui] '" + pack.name
                            + "' compile queued from stale output; applied state retained"
                        );
                        snapshotParameters();
                        notice(tr("Compiling..."), false);
                        return true;
                    }
                    compileAppliedImmediately = true;
                } else if (mHost.reload) {
                    if (!mHost.reload(pack)) {
                        // 图形模式/设备能力不允许这个包，保持"未启用"并提示原因。
                        notice(tr("Deferred shader could not load (Vibrant Visuals unavailable)"), true);
                        log("[ui] '" + pack.name + "' refused: graphics mode unavailable");
                        return false;
                    }
                }
            }
            mAppliedPack    = mPack;
            mEnabledApplied = true;
            mNotice.clear();
            log("[ui] shaders enabled: " + pack.name);
            // 记住这次用的包：重启游戏后按 config.json 自动挂回来。
            if (mHost.rememberSelection) mHost.rememberSelection(pack.name, true);
        }
    }

    if (paramsChanged) notice(compileStarted ? tr("Compiling...") : tr("Settings saved"), false);
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
        auto it    = std::find(p.values.begin(), p.values.end(), p.current);
        int  idx   = it == p.values.end() ? 0 : static_cast<int>(std::distance(p.values.begin(), it));
        int  count = static_cast<int>(p.values.size());
        idx        = (idx + delta) % count;
        if (idx < 0) idx += count;
        next = p.values[static_cast<std::size_t>(idx)];
        break;
    }
    case ParameterType::Color:
        return;
    }

    ShaderPackStore::getInstance().setParameterValue(pack.name, (std::size_t)mRow, std::move(next));
}

// ---------------------------------------------------------------- 设置页
// Iris 风格自定义参数界面：参数来自光影包真实声明的 shaders.properties。

void Screen::enterCompatibility() {
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) {
        notice(tr("Select a shader pack first"), true);
        return;
    }

    auto const& pack = packs[(std::size_t)mPack];
    mCompatibilityPack = packLabel(pack);
    mCompatibility     = mHost.preflight ? mHost.preflight(pack) : CompatibilityReport{};
    mCompatibilityFilter = CompatibilityIssueFilter::Errors;
    if (mCompatibility.nativeIO.errorIssueCount() == 0
        && mCompatibility.nativeIO.warningIssueCount() > 0) {
        mCompatibilityFilter = CompatibilityIssueFilter::Warnings;
    } else if (mCompatibility.nativeIO.errorIssueCount() == 0
               && mCompatibility.nativeIO.warningIssueCount() == 0
               && !mCompatibility.nativeIO.overriddenIssues.empty()) {
        mCompatibilityFilter = CompatibilityIssueFilter::Overridden;
    }
    mCompatibilityScroll         = 0.f;
    mCompatibilityMaxScroll      = 0.f;
    mCompatibilityScrollDragging = false;
    mCompatibilityGrabDy         = 0.f;
    mMode              = Mode::Compatibility;
    mFocus             = Focus::Content;
    mNotice.clear();
    log(
        "[ui] compatibility preflight: pack='" + pack.name
        + "' preset='" + pack.currentPreset
        + "' bins='" + pack.binsDir.string()
        + "' compiled=" + (mCompatibility.compiled ? "true" : "false")
        + " materials=" + std::to_string(mCompatibility.materialCount)
        + " ok=" + (mCompatibility.ok ? "true" : "false")
        + " errors=" + std::to_string(mCompatibility.nativeIO.errors)
        + " warnings=" + std::to_string(mCompatibility.nativeIO.warnings)
        + " overridden=" + std::to_string(mCompatibility.nativeIO.overriddenIssues.size())
    );
}

void Screen::leaveCompatibility() {
    mMode = Mode::List;
    mFocus = Focus::Content;
    mCompatibilityScrollDragging = false;
    mNotice.clear();
}

std::vector<NativeIOIssue> const& Screen::compatibilityIssues() const {
    if (mCompatibilityFilter == CompatibilityIssueFilter::Overridden) {
        return mCompatibility.nativeIO.overriddenIssues;
    }
    return mCompatibility.nativeIO.issues;
}

void Screen::exportCompatibility() {
    if (!mCompatibility.nativeIO.available || mCompatibility.nativeIO.json.empty()) {
        notice(tr("No compatibility report to export"), true);
        return;
    }
    auto const& packs = ShaderPackStore::getInstance().packs();
    if (mPack < 0 || (std::size_t)mPack >= packs.size()) {
        notice(tr("Report export failed"), true);
        return;
    }

    std::string outputPath;
    if (!mHost.exportCompatibilityReport
        || !mHost.exportCompatibilityReport(
            packs[(std::size_t)mPack],
            mCompatibility.nativeIO.json,
            outputPath
        )) {
        log("[ui] compatibility report export failed");
        notice(tr("Report export failed"), true);
        return;
    }

    click();
    log("[ui] compatibility report exported: " + outputPath);
    std::string shownName = std::filesystem::path(outputPath).filename().string();
    if (shownName.empty()) shownName = outputPath;
    notice(tr("Report exported") + ": " + shownName, false);
}

void Screen::enterSettings() {
    mMode = Mode::Settings;

    auto const& packs   = ShaderPackStore::getInstance().packs();
    bool const  hasPack = (mPack >= 0 && (std::size_t)mPack < packs.size());
    std::string const packLocale = mHost.locale ? mHost.locale() : std::string{};
    mSettings           = hasPack ? settingsFromPack(packs[(std::size_t)mPack], packLocale) : SettingsModel{};
    mSettings.title     = hasPack ? packLabel(packs[(std::size_t)mPack]) : tr("Shader Packs");
    mSettings.subtitle  = "Configure";

    // 有分类（screen.*）就先进分类页（Iris 的第一层）；没有分类就直接给一页参数。
    std::vector<int> const top = settingsTopLevelView(mSettings);
    if (!top.empty()) {
        mSettingsPage = -1;
        mSettingsView = top;
    } else {
        mSettingsPage = -2; // -2 = “整页参数”（没有分类页可返回）
        mSettingsView = settingsFlatView(mSettings);
    }
    mSettingIndex    = settingsFirstSelectable(mSettings, mSettingsView);
    mSettingScroll   = 0.f;
    mSettingDragging = false;
    mFocus           = Focus::Content;
    mFocusIndex      = 0;
    mHeaderRestore   = 0;
    mKeyCapture      = false;
    mPresetBrowserOpen  = false;
    mPresetBrowserFirst = 0;
    mPresetBrowserIndex = 0;
    if (hasPack) {
        auto const& presets = packs[(std::size_t)mPack].presets;
        for (int i = 0; i < (int)presets.size(); ++i) {
            if (presets[(std::size_t)i].id == packs[(std::size_t)mPack].currentPreset) {
                mPresetBrowserIndex = i;
                break;
            }
        }
    }
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
    mFocus           = Focus::Content;
    mHeaderRestore   = 0;
    mKeyCapture      = false;
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
    mFocus           = Focus::Content;
    mFocusIndex      = 0;
    mKeyCapture      = false;
    mPresetBrowserOpen = false;
    mPresetRenameActive = false;
    mSettingDragging = false;
}

bool Screen::settingsDirty() const { return dumpSettings(mSettings) != mSettingsSnapshot; }

// ---------------------------------------------------------------- 模组设置页
//
// 底栏左下角「设置」按钮打开的那一页。内容是模组自己的开关（不是光影包参数）：
//   面板热键（按一个键来绑定）/ 强制开启灵动视效 / 启动时自动加载上次光影 / 图形模式覆盖。
// 值都落在 config.json 里，由宿主（ShaderManagerScreen）通过 readSetting / writeSetting 代理。

namespace {
// 模组设置页没有分类层级：所有非分类项直接铺成一页参数行。
std::vector<int> allSettingItems(SettingsModel const& model) {
    std::vector<int> view;
    for (int i = 0; i < (int)model.items.size(); ++i) {
        if (model.items[(std::size_t)i].kind != SettingKind::Category) view.push_back(i);
    }
    return view;
}
} // namespace

void Screen::buildPreferencesModel() {
    auto read = [this](std::string const& key) -> std::string {
        return mHost.readSetting ? mHost.readSetting(key) : std::string{};
    };

    SettingsModel model;
    model.title    = tr("Settings");
    model.subtitle = tr("Options");

    {
        SettingItem item;
        item.kind     = SettingKind::KeyBind;
        item.span     = SettingSpan::Half;
        item.key      = "uiKey";
        item.label    = "Panel hotkey";
        item.value    = read("uiKey");
        item.defValue = "G";
        model.items.push_back(std::move(item));
    }
    {
        SettingItem item;
        item.kind     = SettingKind::Bool;
        item.span     = SettingSpan::Half;
        item.key      = "forceVibrantVisuals";
        item.label    = "Force vibrant visuals";
        item.value    = isOnValue(read("forceVibrantVisuals")) ? "ON" : "OFF";
        item.defValue = "ON";
        model.items.push_back(std::move(item));
    }
    {
        SettingItem item;
        item.kind     = SettingKind::Bool;
        item.span     = SettingSpan::Half;
        item.key      = "autoLoadLastShader";
        item.label    = "Auto-load last shader pack";
        item.value    = isOnValue(read("autoLoadLastShader")) ? "ON" : "OFF";
        item.defValue = "ON";
        model.items.push_back(std::move(item));
    }
    {
        SettingItem item;
        item.kind     = SettingKind::Bool;
        item.span     = SettingSpan::Half;
        item.key      = "enableCompilation";
        item.label    = "Enable compilation";
        item.value    = isOnValue(read("enableCompilation")) ? "ON" : "OFF";
        item.defValue = "ON";
        model.items.push_back(std::move(item));
    }
    {
        SettingItem item;
        item.kind     = SettingKind::Enum;
        item.span     = SettingSpan::Half;
        item.key      = "graphicsModeOverride";
        item.label    = "Graphics mode override";
        item.value    = read("graphicsModeOverride");
        if (item.value.empty()) item.value = "0";
        item.defValue = "0";
        auto const add = [&item](char const* value, char const* label) {
            item.options.push_back(SettingOption{value, label});
        };
        add("0", "Auto");
        add("1", "Fancy (Forward)");
        add("2", "Vibrant Visuals (Deferred)");
        model.items.push_back(std::move(item));
    }

    mSettings = std::move(model);
}

void Screen::enterPreferences() {
    mMode = Mode::Preferences;
    buildPreferencesModel();

    mSettingsPage    = -2; // -2 = 整页参数（模组设置页没有分类页）
    mSettingsView    = allSettingItems(mSettings);
    mSettingIndex    = settingsFirstSelectable(mSettings, mSettingsView);
    mSettingScroll   = 0.f;
    mSettingDragging = false;
    mFocus           = Focus::Content;
    mFocusIndex      = 0;
    mHeaderRestore   = 0;
    mKeyCapture      = false;
    mNotice.clear();

    mSettingsSnapshot = dumpSettings(mSettings);
    mSettingsLayout   = buildSettingsLayout(
        mLastWidth,
        mLastHeight,
        mSettings,
        mSettingsView,
        SettingPageStyle::Rows,
        -1,
        mSettingScroll
    );
    log("[ui] preferences page: " + std::to_string(mSettings.items.size()) + " item(s)");
}

void Screen::leavePreferences() {
    mMode            = Mode::List;
    mRow             = mPack >= 0 ? mPack + 1 : 0;
    mFocus           = Focus::Content;
    mFocusIndex      = 0;
    mKeyCapture      = false;
    mSettingDragging = false;
}

bool Screen::preferencesDirty() const { return dumpSettings(mSettings) != mSettingsSnapshot; }

// 「应用」：把这一页的值逐个写回 config.json（宿主负责解释 ON/OFF、0..2、键名）。
void Screen::applyPreferences() {
    if (!preferencesDirty()) return;
    bool ok = true;
    for (auto const& item : mSettings.items) {
        if (item.kind == SettingKind::Category) continue;
        if (!mHost.writeSetting || !mHost.writeSetting(item.key, item.value)) ok = false;
    }
    mSettingsSnapshot = dumpSettings(mSettings);
    notice(ok ? tr("Settings saved") : tr("Some settings could not be saved"), !ok);
    log(std::string("[ui] preferences applied: ") + (ok ? "ok" : "partial"));
}

void Screen::discardPreferences() {
    discardSettingsEdits();
    mKeyCapture = false;
}

void Screen::applySettingsEdits() {
    if (!settingsDirty()) return; // 没有改动：点「应用」什么都不做（和列表页一致）

    auto const& packs   = ShaderPackStore::getInstance().packs();
    bool const  hasPack = (mPack >= 0 && (std::size_t)mPack < packs.size());
    if (!hasPack) {
        notice(tr("Select a shader pack first"), true);
        return;
    }

    auto const& pack = packs[(std::size_t)mPack];
    for (auto const& item : mSettings.items) {
        if (item.kind == SettingKind::Category) continue;
        ShaderPackStore::getInstance().setParameterValueByName(pack.name, item.key, item.value);
    }
    ShaderPackStore::saveSettings(pack);

    auto const compileResult =
        mHost.compile ? mHost.compile(pack) : CompileRequestResult::Failed;
    if (compileResult == CompileRequestResult::Failed) {
        notice(tr("Compile could not start"), true);
        return;
    }
    if (compileResult == CompileRequestResult::AppliedImmediately) {
        mEnabledApplied = true;
        mAppliedPack    = mPack;
        if (mHost.rememberSelection) mHost.rememberSelection(pack.name, true);
        log("[ui] settings applied without async compile: " + pack.name);
        snapshotParameters();
        mSettingsSnapshot = dumpSettings(mSettings);
        notice(tr("Settings saved"), false);
        return;
    }

    // 设置页的「应用」同样会启用/切换当前包。这里必须像列表页 applyPending()
    // 一样提交已生效选择；否则关闭面板时 discardPending() 会把 mPack 回滚成旧包，
    // 再打开就会显示上一个包，而实际刚编译、应用的是当前包。
    // 参数编译成功前只保留 pending 状态；成功回调负责提交 applied 状态并
    // 记住选择，避免 IO 门禁失败时 UI 误显示为已启用。
    mEnabled = true;
    log("[ui] settings submitted for compile: " + pack.name);

    snapshotParameters();
    mSettingsSnapshot = dumpSettings(mSettings);
    notice(tr("Applying..."), false);
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
    if (mCompatibilityScrollDragging) {
        if (mCompatibilityMaxScroll > 0.f
            && mCompatibilityScrollTrack.valid()
            && mCompatibilityScrollHandle.valid()) {
            mCompatibilityScroll = scrollFromHandle(
                y,
                mCompatibilityGrabDy,
                mCompatibilityScrollTrack,
                mCompatibilityScrollHandle,
                mCompatibilityMaxScroll
            );
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

// 焦点回到内容区（列表行 / 参数行）。
void Screen::focusContent() { mFocus = Focus::Content; }

// 按钮区（底栏 5 个 + 右下角预览 + 左下角设置）里的方向键移动：按几何位置找"那个方向最近的那个"。
// 用户反馈的两个问题都在这里收口：
//   ① 以前底栏上排是 `mButtonFocus - 2`，从「完成」往上会跳到「取消」，而它正上方其实是
//      「光影包设置…」——现在按矩形位置选（上下重叠优先），「完成」上就是「光影包设置…」；
//   ② 右下角「全透明」和左下角「设置」以前根本进不去，现在它们是同一张候选表里的按钮。
bool Screen::moveButtonFocus(int dx, int dy) {
    if (dx == 0 && dy == 0) return false;

    struct Candidate {
        Focus zone;
        int   index;
        Rect  rect;
    };

    auto const&            L = mLayout;
    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(L.buttonCount) + 4);
    for (int i = 0; i < L.buttonCount; ++i) candidates.push_back({Focus::Bottom, i, L.buttons[i]});
    if (L.previewButton.valid()) candidates.push_back({Focus::Preview, 0, L.previewButton});
    if (L.settingsButton.valid()) candidates.push_back({Focus::SettingsButton, 0, L.settingsButton});
    if (mMode == Mode::List && L.compatibilityButton.valid()) {
        candidates.push_back({Focus::CompatibilityButton, 0, L.compatibilityButton});
    }

    // 当前焦点所在的矩形；找不到（比如刚进界面）就退回底栏第一个按钮。
    Rect current{};
    for (auto const& c : candidates) {
        if (c.zone == mFocus && c.index == mFocusIndex) {
            current = c.rect;
            break;
        }
    }
    if (!current.valid()) {
        if (mFocus == Focus::Preview) current = L.previewButton;
        else if (mFocus == Focus::SettingsButton) current = L.settingsButton;
        else if (mFocus == Focus::CompatibilityButton) current = L.compatibilityButton;
        else current = (mFocusIndex >= 0 && mFocusIndex < L.buttonCount) ? L.buttons[mFocusIndex] : Rect{};
    }
    if (!current.valid()) return false;

    float const curX = (current.x0 + current.x1) / 2.f;
    float const curY = (current.y0 + current.y1) / 2.f;

    // 垂直方向看 x 轴是否重叠，反之亦然：重叠 = 0 分，不重叠按空隙扣分（3 倍权重，
    // 保证"正上方/正下方"优先于"斜对角"）。
    auto gap = [](float a0, float a1, float b0, float b1) {
        if (a1 < b0) return b0 - a1;
        if (b1 < a0) return a0 - b1;
        return 0.f;
    };

    // 两轮挑选：
    //   第一轮只认"在那个方向上、而且与当前按钮在垂直轴上有重叠"的（同一排 / 同一个角落），
    //   按主轴距离取最近的 —— 这一步保证「取消」往右走是「应用」而不是"斜上方的打开文件夹"；
    //   第二轮才允许斜向（比如右下角按钮往上走）：按主轴距离 + 3 倍垂直空隙挑最近的。
    bool      foundOverlap = false;
    float     bestOverlap  = 0.f;
    Candidate overlapC{};
    bool      foundAny = false;
    float     bestAny  = 0.f;
    Candidate anyC{};
    for (auto const& c : candidates) {
        if (c.zone == mFocus && c.index == mFocusIndex) continue;
        if (!c.rect.valid()) continue;

        float const cx = (c.rect.x0 + c.rect.x1) / 2.f;
        float const cy = (c.rect.y0 + c.rect.y1) / 2.f;
        float       primary;
        float       perpGap;
        if (dx != 0) {
            primary = (cx - curX) * (float)dx;
            if (primary <= 0.5f) continue;
            perpGap = gap(current.y0, current.y1, c.rect.y0, c.rect.y1);
        } else {
            primary = (cy - curY) * (float)dy;
            if (primary <= 0.5f) continue;
            perpGap = gap(current.x0, current.x1, c.rect.x0, c.rect.x1);
        }
        float const score = primary + perpGap * 3.f;
        if (!foundAny || score < bestAny) {
            foundAny = true;
            bestAny  = score;
            anyC     = c;
        }
        if (perpGap <= 0.f && (!foundOverlap || primary < bestOverlap)) {
            foundOverlap = true;
            bestOverlap  = primary;
            overlapC     = c;
        }
    }
    if (!foundAny) return false;
    Candidate const& chosen = foundOverlap ? overlapC : anyC;
    mFocus      = chosen.zone;
    mFocusIndex = chosen.index;
    return true;
}

// 设置页（光影包参数 / 模组设置）内容区的键盘操作。
bool Screen::handleSettingsKey(int keyCode) {
    switch (keyCode) {
    case 0x1B: // Esc = 「返回」：先回上一层，顶层再回光影包列表
        if (mMode == Mode::Preferences) leavePreferences();
        else settingsGoBack();
        return true;
    case 0x26: { // Up：已经在第一项就上到顶栏四个按钮
        int const before = mSettingIndex;
        moveSettingSelection(0, -1);
        if (mSettingIndex == before) {
            mHeaderRestore = (mHeaderRestore < 0) ? 0 : mHeaderRestore;
            mFocus         = Focus::Header;
            mFocusIndex    = mHeaderRestore;
        }
        return true;
    }
    case 0x28: { // Down：到底了就交给底栏按钮
        int const before = mSettingIndex;
        moveSettingSelection(0, 1);
        if (mSettingIndex == before) {
            mFocus      = Focus::Bottom;
            mFocusIndex = 2;
        }
        return true;
    }
    case 0x25: // Left
        moveSettingSelection(-1, 0);
        return true;
    case 0x27: // Right
        moveSettingSelection(1, 0);
        return true;
    case 0x0D: // Enter：分类=下一层；键位=开始录键；其它=改值
        if (mSettingIndex >= 0 && (std::size_t)mSettingIndex < mSettings.items.size()) {
            auto const& item = mSettings.items[(std::size_t)mSettingIndex];
            click();
            if (item.kind == SettingKind::Category) {
                openSettingsCategory(mSettingIndex);
            } else if (item.kind == SettingKind::KeyBind) {
                mKeyCapture = true;
                notice(tr("Press a key..."), false);
            } else {
                settingsAdjust(mSettings, mSettingIndex, 1);
            }
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

// 列表页内容区。
bool Screen::handleListKey(int keyCode) {
    switch (keyCode) {
    case 0x1B:            // Escape
        discardPending(); // 和「取消」一样：丢掉没应用的改动
        close();
        return true;
    case 0x26: // Up
        if (mRow > 0) {
            --mRow;
            listSyncToRow();
        } else {
            mFocus = Focus::CompatibilityButton;
        }
        return true;
    case 0x28: { // Down：到底了就把焦点交给底栏
        int const count = contentCount();
        if (mRow + 1 < count) {
            ++mRow;
            listSyncToRow();
        } else {
            mFocus      = Focus::Bottom;
            mFocusIndex = 2;
        }
        return true;
    }
    case 0x0D: // Enter
        click();
        activateRow(mRow);
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
    default:
        return true;
    }
}

// 设置页顶栏（返回 / 保存 / 导入 / 导出 / 重置）。以前只有鼠标点得到，方向键上不去。
bool Screen::handleHeaderKey(int keyCode) {
    bool const hasFileButtons = (mMode == Mode::Settings);
    int const  lastIndex      = hasFileButtons ? 4 : 0;
    switch (keyCode) {
    case 0x1B: // Esc：和「返回」一样
        if (mMode == Mode::Preferences) leavePreferences();
        else settingsGoBack();
        return true;
    case 0x25: // Left
        mFocusIndex = std::max(mFocusIndex - 1, 0);
        mHeaderRestore = mFocusIndex;
        return true;
    case 0x27: // Right
        mFocusIndex = std::min(mFocusIndex + 1, lastIndex);
        mHeaderRestore = mFocusIndex;
        return true;
    case 0x28: // Down：回到参数行
        focusContent();
        return true;
    case 0x0D: // Enter：按下顶栏按钮
        click();
        if (mFocusIndex == 0) {
            if (mMode == Mode::Preferences) leavePreferences();
            else settingsGoBack();
        } else if (mMode == Mode::Settings) {
            if (mFocusIndex == 1) saveCurrentConfiguration();
            else if (mFocusIndex == 2) importSettingsFile();
            else if (mFocusIndex == 3) exportSettingsFile();
            else resetCurrentConfiguration();
        }
        return true;
    default:
        return true;
    }
}

bool Screen::handleBottomKey(int keyCode) {
    switch (keyCode) {
    case 0x1B: // Esc：焦点先回内容区（再按一次才是"返回/关闭"）
        focusContent();
        return true;
    case 0x26: // Up：下排 -> 上排（按位置选）；上排 -> 内容区
        if (mFocusIndex < 2 || !moveButtonFocus(0, -1)) focusContent();
        return true;
    case 0x28: // Down：上排 -> 下排（按位置选）
        moveButtonFocus(0, 1);
        return true;
    case 0x25: // Left：底栏内往左；在最左边就是左下角的「设置」
        moveButtonFocus(-1, 0);
        return true;
    case 0x27: // Right：底栏内往右；在最右边就是右下角的「全透明」
        moveButtonFocus(1, 0);
        return true;
    case 0x0D: // Enter
        click();
        activateButton(mFocusIndex);
        return true;
    default:
        return true;
    }
}

bool Screen::handlePreviewKey(int keyCode) {
    switch (keyCode) {
    case 0x1B:
        focusContent();
        return true;
    case 0x0D: // Enter：进入「全透明」预览模式
        click();
        mTransparentPreview = true;
        log("[ui] transparent preview on");
        return true;
    case 0x26:
    case 0x28:
    case 0x25:
        moveButtonFocus(0, -1);
        if (mFocus == Focus::Preview) moveButtonFocus(-1, 0);
        return true;
    case 0x27:
        moveButtonFocus(-1, 0);
        return true;
    default:
        return true;
    }
}

bool Screen::handleCompatibilityButtonKey(int keyCode) {
    switch (keyCode) {
    case 0x1B:
    case 0x28:
        focusContent();
        return true;
    case 0x0D:
        click();
        enterCompatibility();
        return true;
    case 0x25:
    case 0x26:
    case 0x27:
        moveButtonFocus(keyCode == 0x25 ? -1 : 1, keyCode == 0x26 ? -1 : 0);
        return true;
    default:
        return true;
    }
}

bool Screen::handleCompatibilityKey(int keyCode) {
    if (keyCode == 0x1B || keyCode == 0x0D) {
        click();
        leaveCompatibility();
        return true;
    }
    auto selectFilter = [&](int index) {
        index = (index + 4) % 4;
        mCompatibilityFilter    = (CompatibilityIssueFilter)index;
        mCompatibilityScroll    = 0.f;
        mCompatibilityMaxScroll = 0.f;
        click();
    };
    float const viewportHeight =
        std::max(0.f, mCompatibilityViewport.y1 - mCompatibilityViewport.y0);
    float const page = std::max(18.f, viewportHeight - 18.f);
    switch (keyCode) {
    case 0x09:
    case 0x27:
        selectFilter((int)mCompatibilityFilter + 1);
        break;
    case 0x25:
        selectFilter((int)mCompatibilityFilter - 1);
        break;
    case 0x26:
        mCompatibilityScroll = std::max(0.f, mCompatibilityScroll - 18.f);
        break;
    case 0x28:
        mCompatibilityScroll =
            std::min(mCompatibilityMaxScroll, mCompatibilityScroll + 18.f);
        break;
    case 0x21:
        mCompatibilityScroll = std::max(0.f, mCompatibilityScroll - page);
        break;
    case 0x22:
        mCompatibilityScroll = std::min(mCompatibilityMaxScroll, mCompatibilityScroll + page);
        break;
    case 0x24:
        mCompatibilityScroll = 0.f;
        break;
    case 0x23:
        mCompatibilityScroll = mCompatibilityMaxScroll;
        break;
    case 0x53: // S
        exportCompatibility();
        break;
    default:
        break;
    }
    return true;
}

bool Screen::handleSettingsButtonKey(int keyCode) {
    switch (keyCode) {
    case 0x1B:
        focusContent();
        return true;
    case 0x0D: // Enter：打开模组设置页
        click();
        enterPreferences();
        return true;
    case 0x27: // Right：回到取消按钮（底栏最左边那个）
        mFocus      = Focus::Bottom;
        mFocusIndex = 2;
        return true;
    case 0x26:
    case 0x28:
        moveButtonFocus(0, keyCode == 0x26 ? -1 : 1);
        if (mFocus == Focus::SettingsButton) focusContent();
        return true;
    default:
        return true;
    }
}

// 设置页录键位：按下的第一个键就是新的面板热键。
bool Screen::handleKeyCapture(int keyCode) {
    if (keyCode == 0x1B) { // Esc 取消
        mKeyCapture = false;
        notice(tr("Key binding cancelled"), true);
        return true;
    }
    // 回车 / Tab 是界面自己的操作键，不能拿来当面板热键。
    if (keyCode == 0x0D || keyCode == 0x09) {
        notice(tr("That key cannot be used"), true);
        return true;
    }
    std::string name = hotkeyNameFromVk(keyCode);
    if (name.empty()) {
        notice(tr("Unsupported key"), true);
        return true;
    }
    if (mSettingIndex >= 0 && (std::size_t)mSettingIndex < mSettings.items.size()) {
        mSettings.items[(std::size_t)mSettingIndex].value = name;
    }
    mKeyCapture = false;
    notice(tr("Panel hotkey") + ": " + name + " (" + tr("press Apply to save") + ")", false);
    return true;
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
    mPack    = packIndex;
    mRow     = index;
    mEnabled = true;
    focusContent();
    snapshotParameters(); // 快照换成这个包的参数，之后调参才算"改动"
}

void Screen::activateButton(int index) {
    switch (index) {
    case 0: // 打开光影包文件夹
        if (mHost.openFolder) mHost.openFolder();
        return;
    case 1: // 光影包设置... / 光影包列表...
        if (mMode == Mode::List) enterSettings();
        else if (mMode == Mode::Preferences) leavePreferences();
        else leaveSettings();
        return;
    case 2: // 取消：设置页/模组设置页 = 回列表；列表页 = 丢掉改动并退出
        if (mMode == Mode::Preferences) {
            discardPreferences();
            leavePreferences();
            return;
        }
        if (mMode == Mode::Settings) {
            discardSettingsEdits();
            leaveSettings();
            return;
        }
        discardPending();
        close();
        return;
    case 3: // 应用：把暂存的改动落地（加载/还原/存盘），界面保持不变
        if (mMode == Mode::Preferences) {
            applyPreferences();
            return;
        }
        if (mMode == Mode::Settings) {
            applySettingsEdits();
            return;
        }
        applyPending();
        return;
    case 4: // 完成 = 应用 + 退出面板。**没有改动 / 不用编译也要关掉**
        // （用户反馈：编译暂停时点「完成」没反应，面板应该照样关）。
        if (mMode == Mode::Preferences) {
            applyPreferences();
            leavePreferences();
            return;
        }
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

    // 透明预览：只保留右下角按钮（也可以用回车/ Esc 退出来 —— 方向键现在能走到这个按钮，
    // 键盘进来的人必须有键盘出去的路）。
    if (mTransparentPreview) {
        if (keyCode == 0x0D || keyCode == 0x1B) {
            mTransparentPreview = false;
            focusContent();
            log("[ui] transparent preview off");
        }
        return true;
    }

    // 编译中锁住界面。
    if (blocked()) return true;

    // 设置页正在等玩家按一个键来绑定：这一下只用来录键位。
    if (mKeyCapture) return handleKeyCapture(keyCode);
    if (mMode == Mode::Settings && mPresetBrowserOpen) {
        if (mPresetRenameActive) return handlePresetRenameKey(keyCode);
        if (mFocus == Focus::Bottom) return handlePresetBrowserBottomKey(keyCode);
        return handlePresetBrowserKey(keyCode);
    }
    if (mMode == Mode::Compatibility) return handleCompatibilityKey(keyCode);

    log("[ui] key " + std::to_string(keyCode) + " mode=" + std::to_string((int)mMode) + " row=" + std::to_string(mRow)
        + " rowCount=" + std::to_string(mLayout.rowCount));

    // 先按"焦点在哪个区"分发：每个区有自己的方向键规则，到边界会跨区。
    switch (mFocus) {
    case Focus::Header:
        return handleHeaderKey(keyCode);
    case Focus::Bottom:
        return handleBottomKey(keyCode);
    case Focus::Preview:
        return handlePreviewKey(keyCode);
    case Focus::SettingsButton:
        return handleSettingsButtonKey(keyCode);
    case Focus::CompatibilityButton:
        return handleCompatibilityButtonKey(keyCode);
    case Focus::Content:
        break;
    }

    // 内容区：列表行 / 设置项各有自己的一套走位。
    if (mMode == Mode::Settings || mMode == Mode::Preferences) return handleSettingsKey(keyCode);
    return handleListKey(keyCode);
    // 注意：这里【不再】给 1..5 绑定五个按钮。之前绑了，结果在界面打开时按数字键
    // 会直接把「打开光影包文件夹」「应用（编译）」这些动作触发掉，打字也会中招。
    // 五个按钮只用鼠标左键或方向键 + 回车操作。
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

    if (mMode == Mode::Compatibility) {
        mCompatibilityScroll = std::clamp(
            mCompatibilityScroll - (float)dir * 30.f,
            0.f,
            mCompatibilityMaxScroll
        );
        return true;
    }

    if (mMode == Mode::Settings || mMode == Mode::Preferences) {
        if (mMode == Mode::Settings && mPresetBrowserOpen) {
            auto const& packs = ShaderPackStore::getInstance().packs();
            if (mPack < 0 || (std::size_t)mPack >= packs.size()) return true;
            int const totalRows = ((int)packs[(std::size_t)mPack].presets.size() + 1) / 2;
            int const maxFirst  = std::max(totalRows - presetBrowserVisibleRows(), 0);
            mPresetBrowserFirst = std::clamp(mPresetBrowserFirst - dir, 0, maxFirst);
            return true;
        }
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
    if ((mSettingScrollDragging || mListScrollDragging || mCompatibilityScrollDragging) && down) {
        updateScrollDrags(y);
    }
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

    if (mMode == Mode::Compatibility) {
        if (!down && wasDown) {
            mCompatibilityScrollDragging = false;
            return true;
        }
        if (down && !wasDown) {
            for (int i = 0; i < 4; ++i) {
                if (!mCompatibilityTabs[i].contains(hx, hy)) continue;
                mCompatibilityFilter    = (CompatibilityIssueFilter)i;
                mCompatibilityScroll    = 0.f;
                mCompatibilityMaxScroll = 0.f;
                click();
                return true;
            }
            if (L.buttons[2].contains(hx, hy)) {
                click();
                leaveCompatibility();
                return true;
            }
            if (L.buttons[3].contains(hx, hy)) {
                exportCompatibility();
                return true;
            }
            if (mCompatibilityScrollTrack.valid()
                && mCompatibilityScrollHandle.valid()
                && mCompatibilityScrollTrack.contains(hx, hy)) {
                mCompatibilityScrollDragging = true;
                mCompatibilityGrabDy = hy - mCompatibilityScrollHandle.y0;
                updateScrollDrags(hy);
                return true;
            }
        }
        return true;
    }

    if (mMode == Mode::List && down && !wasDown && L.compatibilityButton.contains(hx, hy)) {
        click();
        enterCompatibility();
        return true;
    }

    if (mMode == Mode::Settings && mPresetRenameActive) {
        return true;
    }

    if (mMode == Mode::Settings && mPresetBrowserOpen && down && !wasDown) {
        for (int i = 0; i < L.buttonCount; ++i) {
            if (!L.buttons[i].contains(hx, hy)) continue;
            mFocus      = Focus::Bottom;
            mFocusIndex = i;
            click();
            activatePresetBrowserBottom(i);
            return true;
        }
    }

    // 「配置文件」栏：打开中栏内的两列配置浏览器。
    if (mMode == Mode::Settings && down && !wasDown) {
        if (mPresetRenameActive) return true;
        if (mPresetBrowserOpen) {
            int const firstIndex = mPresetBrowserFirst * 2;
            for (int i = 0; i < (int)mPresetBrowserRects.size(); ++i) {
                if (!mPresetBrowserRects[(std::size_t)i].contains(hx, hy)) continue;
                int const index = firstIndex + i;
                mPresetBrowserIndex = index;
                click();
                return true;
            }
            if (mSettingsLayout.content.contains(hx, hy)) return true;
        } else if (mSettingsLayout.configButton.contains(hx, hy)) {
            mPresetBrowserOpen = true;
            mPresetRenameActive = false;
            auto const& packs = ShaderPackStore::getInstance().packs();
            if (mPack >= 0 && (std::size_t)mPack < packs.size()) {
                auto const& pack = packs[(std::size_t)mPack];
                for (int i = 0; i < (int)pack.presets.size(); ++i) {
                    if (pack.presets[(std::size_t)i].id == pack.currentPreset) {
                        mPresetBrowserIndex = i;
                        break;
                    }
                }
                mPresetBrowserFirst = 0;
            }
            click();
            return true;
        }
    }

    if (down && !wasDown && L.previewButton.contains(hx, hy)) {
        mTransparentPreview = true;
        log("[ui] transparent preview on");
        click();
        return true;
    }

    // 左下角「设置」按钮：打开模组设置页（面板热键 / 强制灵动视效 / 自动加载）。
    if (down && !wasDown && L.settingsButton.contains(hx, hy)) {
        log("[ui] settings button clicked");
        click();
        enterPreferences();
        return true;
    }

    // 滚动条按住拖动：列表页 / 设置页共用。按下时若落在滑块上就记下"指针相对滑块顶端的偏移"
    // （拖起来不跳），落在轨道空白处就把滑块中心挪到指针处。
    if (down && !wasDown) {
        if (mMode == Mode::Settings || mMode == Mode::Preferences) {
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
    if (mMode == Mode::Settings || mMode == Mode::Preferences) {
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
            mFocus      = Focus::Bottom;
            mFocusIndex = i;
            click();
            activateButton(i);
            return true;
        }
    }

    // 设置页：顶栏（返回 + 导入/导出/重置）→ 参数行 → 滚动条。
    if (mMode == Mode::Settings || mMode == Mode::Preferences) {
        SettingsLayout const& S = mSettingsLayout;

        if (S.headerBack.contains(hx, hy)) {
            mFocus         = Focus::Header;
            mFocusIndex    = 0;
            mHeaderRestore = 0;
            log("[ui] settings back");
            click();
            if (mMode == Mode::Settings && mPresetBrowserOpen) {
                mPresetBrowserOpen = false;
                mPresetRenameActive = false;
            }
            else if (mMode == Mode::Preferences) leavePreferences();
            else settingsGoBack(); // 先在设置页内部逐层往上，已经在顶层才回光影包列表
            return true;
        }
        if (mMode == Mode::Settings) {
            for (int i = 0; i < S.headerCount; ++i) {
                if (!S.header[i].contains(hx, hy)) continue;
                mFocus         = Focus::Header;
                mFocusIndex    = i + 1;
                mHeaderRestore = i + 1;
                click();
                if (i == 0) { // 保存当前值并生成新的本地配置文件
                    saveCurrentConfiguration();
                } else if (i == 1) { // 导入配置文件：从外部选一份配置读进来
                    importSettingsFile();
                } else if (i == 2) { // 导出配置文件：把当前值写到外部选的位置
                    exportSettingsFile();
                } else resetCurrentConfiguration();
                log("[ui] settings header button " + std::to_string(i) + " clicked");
                return true;
            }
        }

        SettingPart part = SettingPart::None;
        int const   idx  = settingsHitTest(S, hx, hy, &part);
        if (idx >= 0) {
            mFocus        = Focus::Content;
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
                if (item.kind == SettingKind::KeyBind) {
                    // 点值框 = 开始录键位（也可以用回车）
                    click();
                    mKeyCapture = true;
                    notice(tr("Press a key..."), false);
                } else if (item.kind == SettingKind::Slider) {
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
