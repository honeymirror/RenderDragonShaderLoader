#include "ui/UiSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/ShaderPack.h"

namespace renderdragon_shader_loader::ui {
namespace {

// 版式常量（GUI 单位）。行高比列表页略大，接近 Iris 的设置界面观感。
constexpr float rowH        = 18.f;
constexpr float rowGap      = 2.f;
constexpr float catH        = 18.f;
constexpr float catGap      = 6.f;
constexpr float colGap      = 4.f;
constexpr float contentMaxW = 470.f;
// 顶栏按钮：做小一点、贴着顶栏底下那条分界线（chromeHeaderLineY = 40）摆。
// 「返回」和右边三个**同尺寸**，位置按内容栏的左右缘对称（返回的左边 = 重置的右边关于中线镜像）。
constexpr float headerBtnW   = 46.f;
constexpr float headerBtnH   = 16.f;
constexpr float headerBtnGap = 2.f;
constexpr float headerBtnTop = chromeHeaderLineY - 4.f - headerBtnH; // = 20，底边离分界线 4
constexpr float backBtnW     = headerBtnW;

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(v, hi)); }

std::string formatNumber(double v, bool integer) {
    if (integer) return std::to_string((long long)std::llround(v));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    // 去掉多余的 0，但至少留一位小数（Iris 显示 "1.0" 而不是 "1"）。
    std::string s = buf;
    while (s.size() > 2 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    return s;
}

double parseNumber(std::string const& s, double fallback) {
    char*  end    = nullptr;
    double parsed = std::strtod(s.c_str(), &end);
    return (end != s.c_str() && *end == '\0') ? parsed : fallback;
}

// 一个文本框里：名字在左，值框在右（对齐 Iris：值框都做得很窄）。
float controlWidth(SettingItem const& item, float boxW) {
    switch (item.kind) {
    case SettingKind::Bool:   return std::min(boxW * 0.30f, 50.f);
    case SettingKind::Enum:   return std::min(boxW * 0.48f, 130.f);
    case SettingKind::Color:  return std::min(boxW * 0.24f, 44.f);
    case SettingKind::Slider: return std::min(boxW * 0.30f, 64.f);
    case SettingKind::KeyBind: return std::min(boxW * 0.30f, 60.f);
    default:                  return boxW * 0.5f;
    }
}

bool isCategory(SettingItem const& item) { return item.kind == SettingKind::Category; }

// ---- 造 demo 数据的小工具 ----

SettingItem makeCategory(std::string label) {
    SettingItem item;
    item.kind  = SettingKind::Category;
    item.span  = SettingSpan::Full;
    item.label = std::move(label);
    return item;
}

SettingItem makeBool(std::string key, std::string label, bool on) {
    SettingItem item;
    item.kind     = SettingKind::Bool;
    item.span     = SettingSpan::Half;
    item.key      = std::move(key);
    item.label    = std::move(label);
    item.value    = on ? "ON" : "OFF";
    item.defValue = item.value;
    return item;
}

SettingItem makeSlider(std::string key, std::string label, double min, double max, double def, bool integer,
                       SettingSpan span = SettingSpan::Full) {
    SettingItem item;
    item.kind    = SettingKind::Slider;
    item.span    = span;
    item.key     = std::move(key);
    item.label   = std::move(label);
    item.min     = min;
    item.max     = max;
    item.def     = def;
    item.integer = integer;
    item.value   = formatNumber(def, integer);
    return item;
}

SettingItem makeEnum(std::string key, std::string label, std::string current,
                     std::vector<SettingOption> options, SettingSpan span = SettingSpan::Half) {
    SettingItem item;
    item.kind     = SettingKind::Enum;
    item.span     = span;
    item.key      = std::move(key);
    item.label    = std::move(label);
    item.value    = std::move(current);
    item.defValue = item.value;
    item.options  = std::move(options);
    return item;
}

SettingItem makeColor(std::string key, std::string label, std::string current,
                      std::vector<std::string> palette, SettingSpan span = SettingSpan::Half) {
    SettingItem item;
    item.kind     = SettingKind::Color;
    item.span     = span;
    item.key      = std::move(key);
    item.label    = std::move(label);
    item.value    = std::move(current);
    item.defValue = item.value;
    item.palette  = std::move(palette);
    return item;
}

} // namespace

// 注意：这是**离线工具的测试数据**，游戏内不会用到（游戏里显示的是包真实声明的参数）。
SettingsModel makeDemoSettings() {
    SettingsModel model;
    model.title    = "ExamplePack"; // 示例标题，仅离线预览 / 版式断言用
    model.subtitle = "Configure";

    auto& v = model.items;

    v.push_back(makeCategory("Lighting"));
    v.push_back(makeBool("SUNLIGHT", "Direct Light", true));
    v.push_back(makeBool("AMBIENT", "Ambient Light", true));
    v.push_back(makeSlider("LIGHT_INTENSITY", "Light Intensity", 0.0, 2.0, 1.0, false));
    v.push_back(makeBool("COLORED_LIGHTS", "Colored Lights", false));
    v.push_back(makeSlider("SHADOW_SOFTNESS", "Shadow Softness", 0.0, 100.0, 55.0, true, SettingSpan::Half));

    v.push_back(makeCategory("Sky"));
    v.push_back(makeSlider("STAR_COUNT", "Star Count", 0.0, 5000.0, 1500.0, true));
    v.push_back(makeSlider("STAR_BRIGHTNESS", "Star Brightness", 0.0, 2.0, 1.0, false));
    v.push_back(makeEnum("CLOUD_QUALITY", "Cloud Quality", "Fancy",
                         {{"Off", "Off"}, {"Fast", "Fast"}, {"Fancy", "Fancy"}, {"Ultra", "Ultra"}}));
    v.push_back(makeBool("VOLUMETRIC_CLOUDS", "Volumetric Clouds", true));
    v.push_back(makeSlider("CLOUD_HEIGHT", "Cloud Height", 100.0, 500.0, 180.0, true));

    v.push_back(makeCategory("Water"));
    v.push_back(makeBool("WAVES", "Waves", true));
    v.push_back(makeSlider("WAVE_STRENGTH", "Wave Strength", 0.0, 2.0, 1.0, false, SettingSpan::Half));
    v.push_back(makeSlider("WATER_OPACITY", "Water Opacity", 0.0, 1.0, 0.6, false));
    v.push_back(makeColor("WATER_COLOR", "Water Color", "#3f76e4",
                          {"#3f76e4", "#2e8b57", "#4fa3d1", "#1f5fbf", "#8fd6ff"}));
    v.push_back(makeEnum("WATER_NORMALS", "Water Normals", "Medium",
                         {{"Off", "Off"}, {"Light", "Light"}, {"Medium", "Medium"}, {"Heavy", "Heavy"}}));

    v.push_back(makeCategory("Post Processing"));
    v.push_back(makeBool("BLOOM", "Bloom", true));
    v.push_back(makeSlider("BLOOM_STRENGTH", "Bloom Strength", 0.0, 2.0, 1.0, false, SettingSpan::Half));
    v.push_back(makeEnum("TONEMAP", "Tonemap", "ACES",
                         {{"Vanilla", "Vanilla"}, {"ACES", "ACES"}, {"Uncharted2", "Uncharted2"}, {"Filmic", "Filmic"}}));
    v.push_back(makeBool("CHROMATIC_ABERRATION", "Chromatic Aberration", false));
    v.push_back(makeColor("TINT", "Tint", "#c2b280", {"#c2b280", "#8b5cf6", "#d9534f", "#e0e0e0", "#202020"}));
    v.push_back(makeSlider("EXPOSURE", "Exposure", -2.0, 2.0, 0.0, false, SettingSpan::Half));
    // 后处理这一段故意做得长：真实光影的后处理分类动辄几十项，
    // 这里多造一些整行滑块，用来试"内容超过一屏时的滚动"。
    v.push_back(makeSlider("BLOOM_RADIUS", "Bloom Radius", 0.0, 1.0, 0.5, false));
    v.push_back(makeSlider("BLOOM_THRESHOLD", "Bloom Threshold", 0.0, 1.0, 0.7, false));
    v.push_back(makeSlider("VOLUMETRIC_LIGHT", "Volumetric Light Strength", 0.0, 2.0, 1.0, false));
    v.push_back(makeSlider("LENS_FLARE", "Lens Flare Strength", 0.0, 1.0, 0.25, false));
    v.push_back(makeSlider("VIGNETTE", "Vignette Strength", 0.0, 1.0, 0.35, false));
    v.push_back(makeSlider("SHARPEN", "Sharpen Strength", 0.0, 1.0, 0.2, false));
    v.push_back(makeSlider("SATURATION", "Saturation", 0.0, 2.0, 1.0, false));
    v.push_back(makeSlider("CONTRAST", "Contrast", 0.0, 2.0, 1.0, false));
    v.push_back(makeSlider("BRIGHTNESS", "Brightness", 0.0, 2.0, 1.0, false));
    v.push_back(makeSlider("FILM_GRAIN", "Film Grain Strength", 0.0, 1.0, 0.15, false));
    v.push_back(makeSlider("DISPERSION", "Dispersion Strength", 0.0, 1.0, 0.1, false));
    v.push_back(makeSlider("DOF_STRENGTH", "Depth of Field Strength", 0.0, 1.0, 0.4, false));
    v.push_back(makeSlider("DOF_DISTANCE", "Depth of Field Distance", 0.0, 128.0, 32.0, false));
    v.push_back(makeSlider("MOTION_BLUR", "Motion Blur Strength", 0.0, 1.0, 0.0, false));
    v.push_back(makeSlider("TONEMAP_EXPOSURE", "Tonemap Exposure", -3.0, 3.0, 0.0, false));
    v.push_back(makeSlider("WHITE_BALANCE", "White Balance", -1.0, 1.0, 0.0, false));
    v.push_back(makeSlider("COLOR_TEMPERATURE", "Color Temperature", 2000.0, 12000.0, 6500.0, true));
    v.push_back(makeSlider("GAMMA", "Gamma", 1.0, 3.0, 2.2, false));

    v.push_back(makeCategory("Misc"));
    v.push_back(makeBool("HAND_SHADOW", "Hand Shadow", true));
    v.push_back(makeSlider("HAND_SHADOW_STRENGTH", "Hand Shadow Strength", 0.0, 1.0, 0.95, false,
                           SettingSpan::Half));
    v.push_back(makeEnum("HAND_SHADOW_QUALITY", "Hand Shadow Quality", "Low",
                         {{"Low", "Low"}, {"Medium", "Medium"}, {"High", "High"}}));
    v.push_back(makeEnum("DEBUG_VIEW", "Debug View", "Off",
                         {{"Off", "Off"}, {"Normals", "Normals"}, {"Lighting", "Lighting"}}));

    return model;
}

SettingsModel settingsFromPack(ShaderPack const& pack) {
    SettingsModel model;
    model.title    = pack.name;
    model.subtitle = "Configure";
    for (auto const& p : pack.parameters) {
        SettingItem item;
        item.key   = p.macro.empty() ? p.name : p.macro;
        item.label = p.label.empty() ? p.name : p.label;
        item.value = p.current;
        switch (p.type) {
        case ParameterType::Bool:
            item.kind     = SettingKind::Bool;
            item.span     = SettingSpan::Half;
            item.defValue = item.value;
            break;
        case ParameterType::Slider:
            item.kind    = SettingKind::Slider;
            item.span    = SettingSpan::Full;
            item.min     = p.min;
            item.max     = p.max;
            item.def     = p.def;
            item.integer = p.integer;
            break;
        case ParameterType::Enum:
            item.kind     = SettingKind::Enum;
            item.span     = SettingSpan::Half;
            item.defValue = item.value;
            for (auto const& value : p.values) item.options.push_back({value, value});
            break;
        case ParameterType::Color:
            item.kind     = SettingKind::Color;
            item.span     = SettingSpan::Half;
            item.defValue = item.value;
            break;
        }
        model.items.push_back(std::move(item));
    }
    return model;
}

// ---------------------------------------------------------------- 版式

// 视图里第一个可选中的项；顶层页里分类本身也是可选中的（它是一个按钮）。
int settingsFirstSelectable(SettingsModel const& model, std::vector<int> const& view) {
    for (int index : view) {
        if (index < 0 || (std::size_t)index >= model.items.size()) continue;
        if (!isCategory(model.items[(std::size_t)index])) return index;
    }
    return view.empty() ? -1 : view.front();
}

std::vector<int> settingsTopLevelView(SettingsModel const& model) {
    std::vector<int> view;
    for (int i = 0; i < (int)model.items.size(); ++i) {
        if (isCategory(model.items[(std::size_t)i])) view.push_back(i);
    }
    return view;
}

// 包里只声明了参数、没有 screen.* 分类时用这个"整页参数"视图（按声明顺序）。
std::vector<int> settingsFlatView(SettingsModel const& model) {
    std::vector<int> view;
    for (int i = 0; i < (int)model.items.size(); ++i) {
        if (!isCategory(model.items[(std::size_t)i])) view.push_back(i);
    }
    return view;
}

std::vector<int> settingsCategoryView(SettingsModel const& model, int categoryIndex) {
    std::vector<int> view;
    if (categoryIndex < 0 || (std::size_t)categoryIndex >= model.items.size()) return view;
    for (int i = categoryIndex + 1; i < (int)model.items.size(); ++i) {
        if (isCategory(model.items[(std::size_t)i])) break;
        view.push_back(i);
    }
    return view;
}

std::string settingsCategoryLabel(SettingsModel const& model, int categoryIndex) {
    if (categoryIndex < 0 || (std::size_t)categoryIndex >= model.items.size()) return {};
    return model.items[(std::size_t)categoryIndex].label;
}

SettingsLayout buildSettingsLayout(float width, float height, SettingsModel const& model,
                                   std::vector<int> const& view, SettingPageStyle style, int hovered,
                                   float scrollIn) {
    SettingsLayout L;
    if (width <= 0.f || height <= 0.f) return L;

    // 全部取整：边框和文字落在整数像素上才不糊，左右两格也才是完全一样的尺寸。
    // 和列表页共用同一条列宽规则：整体让开左右两个角落按钮（左下「设置」/ 右下「全透明」），
    // 否则 GUI 宽度小的时候参数行会压到角落按钮上。
    float const contentW = std::floor(contentColumnWidth(width, contentMaxW));
    float const centerX  = std::floor(width / 2.f);
    float const left     = std::floor(centerX - contentW / 2.f);
    float const right    = left + contentW;

    float const colW     = std::floor((contentW - colGap) / 2.f);
    float const colX[2]  = {left, left + colW + colGap};

    float const vpTop    = chromeHeaderLineY + 4.f;
    float const vpBottom = chromeBottomBarTop(height) - 4.f;
    L.content  = {left, chromeHeaderLineY, right, chromeBottomBarTop(height)};
    L.viewport = {left, vpTop, right, vpBottom};
    float const vpH = std::max(vpBottom - vpTop, 1.f);

    // 顶栏按钮：左上角「返回」，右上角 导入 / 导出 / 重置（右边对齐内容栏右缘）。
    L.headerBack = {left, headerBtnTop, left + backBtnW, headerBtnTop + headerBtnH};
    {
        float bx = right;
        for (int i = L.headerCount - 1; i >= 0; --i) {
            L.header[i] = {bx - headerBtnW, headerBtnTop, bx, headerBtnTop + headerBtnH};
            bx -= headerBtnW + headerBtnGap;
        }
    }

    // 第一趟：算出总内容高度（含每个 slot 的行号 / 列号），以及滚动范围。
    struct SlotInfo {
        int   index;
        int   row;
        int   column;
        float y0;
        float h;
        bool  expanded;
        bool  button;
    };
    std::vector<SlotInfo> info;
    info.reserve(view.size());

    int   rowIdx = 0;
    float cursor = 0.f;

    if (style == SettingPageStyle::ButtonGrid) {
        // 顶层分类页：两列按钮网格（对齐 Iris 的分类页）。
        for (int k = 0; k < (int)view.size(); ++k) {
            info.push_back(
                {view[(std::size_t)k], k / 2, k % 2, (float)(k / 2) * (rowH + rowGap), rowH, false, true}
            );
        }
        cursor = (float)(((int)view.size() + 1) / 2) * (rowH + rowGap);
    } else {
        int const n = (int)view.size();
        for (int k = 0; k < n;) {
            int const index = view[(std::size_t)k];
            if (index < 0 || (std::size_t)index >= model.items.size()) {
                ++k;
                continue;
            }
            auto const& item = model.items[(std::size_t)index];
            if (isCategory(item)) {
                info.push_back({index, rowIdx, -1, cursor, catH, false, false});
                cursor += catH + catGap;
                ++rowIdx;
                ++k;
                continue;
            }

            int const nextIndex =
                (k + 1 < n) ? view[(std::size_t)(k + 1)] : -1;
            bool const canPair =
                nextIndex >= 0 && (std::size_t)nextIndex < model.items.size() &&
                !isCategory(model.items[(std::size_t)nextIndex]) &&
                model.items[(std::size_t)nextIndex].span == SettingSpan::Half;

            // 滑块：鼠标悬停就展开（占满它所在的那个文本框），移开立刻复原。
            bool const expanded = item.kind == SettingKind::Slider && index == hovered;

            if (item.span == SettingSpan::Full || !canPair) {
                info.push_back(
                    {index, rowIdx, item.span == SettingSpan::Full ? -1 : 0, cursor, rowH, expanded, false}
                );
                cursor += rowH + rowGap;
                ++rowIdx;
                ++k;
                continue;
            }

            auto const& nextItem = model.items[(std::size_t)nextIndex];
            info.push_back({index, rowIdx, 0, cursor, rowH, expanded, false});
            info.push_back(
                {nextIndex, rowIdx, 1, cursor, rowH,
                 nextItem.kind == SettingKind::Slider && nextIndex == hovered, false}
            );
            cursor += rowH + rowGap;
            ++rowIdx;
            k += 2;
        }
    }
    L.contentHeight = cursor;
    L.maxScroll     = std::max(0.f, cursor - vpH);
    L.scroll        = clampf(scrollIn, 0.f, L.maxScroll);
    float const topShift = vpTop - L.scroll;

    // 第二趟：生成几何。
    L.slots.reserve(info.size());
    for (auto const& si : info) {
        auto const& item = model.items[(std::size_t)si.index];
        SettingBox  box;
        box.index    = si.index;
        box.row      = si.row;
        box.column   = si.column;
        box.expanded = si.expanded;
        box.button   = si.button;

        float const y0 = topShift + si.y0;
        float const y1 = y0 + si.h;

        // 顶层分类按钮：整格就是一个按钮（只画名字，点开进入下一层）。
        if (si.button) {
            box.row_  = {colX[si.column], y0, colX[si.column] + colW, y1};
            box.label = box.row_;
            L.slots.push_back(box);
            continue;
        }

        if (isCategory(item)) {
            box.row_  = {left, y0, right, y1};
            box.label = box.row_;
            L.slots.push_back(box);
            continue;
        }

        if (si.column < 0) {
            box.row_ = (item.span == SettingSpan::Full) ? Rect{left, y0, right, y1} : Rect{colX[0], y0, colX[0] + colW, y1};
        } else {
            box.row_ = {colX[si.column], y0, colX[si.column] + colW, y1};
        }

        float const boxW = box.row_.x1 - box.row_.x0;
        if (si.expanded) {
            // 悬停 / 拖动：整格变成滑块轨道（数值居中、滑标落在数值位置）。
            // 名字**不画在行里**：由 UiScreen 画在鼠标旁边的独立浮层上（对齐 Iris 的
            // SliderElementWidget —— 它只 renderSlider，名字走 GuiUtil.drawTextPanel 的 tooltip）。
            box.control = {box.row_.x0 + 1.f, box.row_.y0 + 1.f, box.row_.x1 - 1.f, box.row_.y1 - 1.f};
            box.label   = box.row_;
        } else {
            float const cw = controlWidth(item, boxW);
            // 值框上下各留 2：内外两层框的边距一致，文字基线也就和左边的名字齐平。
            box.control    = {box.row_.x1 - cw - 3.f, box.row_.y0 + 2.f, box.row_.x1 - 3.f, box.row_.y1 - 2.f};
            box.label      = {box.row_.x0 + 2.f, box.row_.y0, box.control.x0 - 2.f, box.row_.y1};
        }
        L.slots.push_back(box);
    }

    // 可见项：完全落在可视区内的才画 / 才参与命中（避免半行文字溢出滚动区）。
    for (int i = 0; i < (int)L.slots.size(); ++i) {
        auto const& r = L.slots[(std::size_t)i].row_;
        if (r.y0 >= vpTop - 0.5f && r.y1 <= vpBottom + 0.5f) L.visible.push_back(i);
    }

    // 右侧滚动条：内容超出可视区时才显示。Iris 是贴屏幕最右边的一条粗灰条，
    // 所以这里不挂在内容栏上，而是贴屏幕右缘。
    if (L.maxScroll > 0.5f) {
        float const trackTop = vpTop;
        float const trackBot = vpBottom;
        L.scrollTrack  = {width - 7.f, trackTop, width - 1.f, trackBot};
        float const trackH = trackBot - trackTop;
        float const handleH = std::max(18.f, trackH * (vpH / std::max(L.contentHeight, 1.f)));
        float const t       = (L.maxScroll > 0.f) ? (L.scroll / L.maxScroll) : 0.f;
        float const hy0     = trackTop + t * (trackH - handleH);
        L.scrollHandle = {L.scrollTrack.x0, hy0, L.scrollTrack.x1, hy0 + handleH};
    }

    return L;
}

int settingsHitTest(SettingsLayout const& L, float x, float y, SettingPart* part) {
    if (part) *part = SettingPart::None;
    if (!L.viewport.contains(x, y)) return -1;
    for (int slotIndex : L.visible) {
        auto const& box = L.slots[(std::size_t)slotIndex];
        if (box.index < 0) continue;
        // 展开的滑块整格都是轨道（名字框跟着滑标走，也是轨道的一部分，拖哪都行）。
        if (box.control.valid() && box.control.contains(x, y)) {
            if (part) *part = SettingPart::Control;
            return box.index;
        }
        if (box.label.valid() && box.label.contains(x, y)) {
            if (part) *part = SettingPart::Label;
            return box.index;
        }
        if (box.row_.contains(x, y)) {
            if (part) *part = SettingPart::Label;
            return box.index;
        }
    }
    return -1;
}

// ---------------------------------------------------------------- 值操作

bool settingsIsOn(SettingItem const& item) {
    std::string s = item.value;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s == "on" || s == "true" || s == "yes" || s == "1";
}

std::string settingsEnumLabel(SettingItem const& item) {
    for (auto const& option : item.options) {
        if (option.value == item.value) return option.label;
    }
    return item.value;
}

std::string settingsFormatNumber(SettingItem const& item) {
    return formatNumber(parseNumber(item.value, item.def), item.integer);
}

float settingsSliderFraction(SettingItem const& item) {
    double const lo = std::min(item.min, item.max);
    double const hi = std::max(item.min, item.max);
    double const v  = std::clamp(parseNumber(item.value, item.def), lo, hi);
    return (hi > lo) ? (float)((v - lo) / (hi - lo)) : 0.f;
}

std::string settingsValueText(SettingItem const& item, std::string const& on, std::string const& off) {
    switch (item.kind) {
    case SettingKind::Bool:   return settingsIsOn(item) ? on : off;
    case SettingKind::Slider: return settingsFormatNumber(item);
    case SettingKind::Enum:   return settingsEnumLabel(item);
    default:                  return item.value;
    }
}

void settingsAdjust(SettingsModel& model, int index, int delta) {
    if (index < 0 || (std::size_t)index >= model.items.size()) return;
    auto& item = model.items[(std::size_t)index];
    switch (item.kind) {
    case SettingKind::Bool:
        item.value = settingsIsOn(item) ? "OFF" : "ON";
        break;
    case SettingKind::Slider: {
        double const step = item.integer ? 1.0 : std::max((item.max - item.min) / 20.0, 0.001);
        double const cur  = parseNumber(item.value, item.def);
        double       next = cur + step * (double)delta;
        next = std::clamp(next, std::min(item.min, item.max), std::max(item.min, item.max));
        item.value = formatNumber(next, item.integer);
        break;
    }
    case SettingKind::Enum: {
        if (item.options.empty()) break;
        int idx = 0;
        for (int i = 0; i < (int)item.options.size(); ++i) {
            if (item.options[(std::size_t)i].value == item.value) {
                idx = i;
                break;
            }
        }
        int const count = (int)item.options.size();
        idx = ((idx + delta) % count + count) % count;
        item.value = item.options[(std::size_t)idx].value;
        break;
    }
    case SettingKind::Color: {
        if (item.palette.empty()) break;
        int idx = 0;
        for (int i = 0; i < (int)item.palette.size(); ++i) {
            if (item.palette[(std::size_t)i] == item.value) {
                idx = i;
                break;
            }
        }
        int const count = (int)item.palette.size();
        idx = ((idx + delta) % count + count) % count;
        item.value = item.palette[(std::size_t)idx];
        break;
    }
    case SettingKind::Category:
    case SettingKind::KeyBind: // 键位只能「回车/点击」录，方向键不动它
        break;
    }
}

void settingsSetSlider(SettingsModel& model, int index, float t) {
    if (index < 0 || (std::size_t)index >= model.items.size()) return;
    auto& item = model.items[(std::size_t)index];
    if (item.kind != SettingKind::Slider) return;
    double const lo   = std::min(item.min, item.max);
    double const hi   = std::max(item.min, item.max);
    double       v    = lo + (hi - lo) * (double)clampf(t, 0.f, 1.f);
    if (item.integer) v = std::llround(v);
    item.value = formatNumber(v, item.integer);
}

void settingsResetDefaults(SettingsModel& model) {
    for (auto& item : model.items) {
        switch (item.kind) {
        case SettingKind::Slider:
            item.value = formatNumber(item.def, item.integer);
            break;
        case SettingKind::Bool:
        case SettingKind::Enum:
        case SettingKind::Color:
        case SettingKind::KeyBind:
            if (!item.defValue.empty()) item.value = item.defValue;
            break;
        case SettingKind::Category:
            break;
        }
    }
}

int settingsMove(SettingsLayout const& L, int current, int dx, int dy) {
    int   row    = -1;
    int   column = 0;
    float center = 0.f;
    for (auto const& box : L.slots) {
        if (box.index == current) {
            row    = box.row;
            column = box.column;
            center = (box.row_.x0 + box.row_.x1) / 2.f;
            break;
        }
    }
    if (row < 0) return current;

    if (dx != 0) {
        int const want = column + dx;
        for (auto const& box : L.slots) {
            if (box.row == row && box.column == want) return box.index;
        }
        return current;
    }
    if (dy != 0) {
        // 上下移动按"横向中心最接近"来选目标项 —— 这样整行项（column = -1）也能被选中；
        // 以前直接跳过 column < 0 的项，导致"整行滑块下面还有内容"时按 ↓ 完全没反应。
        int const target = row + dy;
        int       best     = -1;
        float     bestDist = 1e9f;
        for (auto const& box : L.slots) {
            if (box.row != target) continue;
            float const dist = std::abs((box.row_.x0 + box.row_.x1) / 2.f - center);
            if (dist < bestDist) {
                bestDist = dist;
                best     = box.index;
            }
        }
        return best >= 0 ? best : current;
    }
    return current;
}

} // namespace renderdragon_shader_loader::ui
