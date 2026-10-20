#pragma once

#include <string>
#include <vector>

#include "ui/UiLayout.h"

namespace renderdragon_shader_loader {
struct ShaderPack; // 前向声明：真实包 -> 设置模型的映射（本期界面用的是自拟假参数）
}

// 光影自定义参数设置界面的「模型 + 版式」，刻意不依赖任何游戏类型：
// 和 UiScreen 一样可以被游戏内与 tools/ui_preview 两个宿主共用。
//
// 对标 Java 版 Iris / OptiFine 的 shaders.properties 参数界面：
//   - 控件：复选框(Bool) / 滑块(Slider) / 设置列表项(Enum) / 颜色(Color)；
//   - 版式：窄控件两列并排，滑块等宽控件占满整行；
//   - 滑块的特色交互：未选中 = 名字在左、滑块在右；选中后滑块放大占满整个文本框。
namespace renderdragon_shader_loader::ui {

// KeyBind = 「按一个键来绑定」的项（模组设置页的面板热键）。值直接存键名（如 G / NONE）。
enum class SettingKind { Category, Bool, Slider, Enum, Color, KeyBind };

// 设置项在版式里占半行（两列并排）还是整行。
enum class SettingSpan { Half, Full };

// 一页怎么排：Rows = 参数行（两列 / 整行）；ButtonGrid = 顶层的分类按钮（两列）。
enum class SettingPageStyle { Rows, ButtonGrid };

struct SettingOption {
    std::string value; // 存进配置的值
    std::string label; // 显示名（i18n key）
};

struct SettingItem {
    SettingKind kind  = SettingKind::Bool;
    SettingSpan span  = SettingSpan::Half;
    std::string key;   // 宏名 / 存储键
    std::string label; // 显示名（i18n key）
    std::string value; // 当前值（字符串形式）

    // Slider
    double min     = 0.0;
    double max     = 1.0;
    double def     = 0.0;
    bool   integer = false;

    // Enum
    std::vector<SettingOption> options;

    // Color：value 是 "#RRGGBB"；点击时在 palette 里循环。
    std::vector<std::string> palette;

    // 非滑块项的默认值（重置为默认配置时回到这里）。
    std::string defValue;
};

struct SettingsModel {
    std::string title;    // 顶栏标题（通常是光影包名）
    std::string subtitle; // 顶栏副标题
    std::vector<SettingItem> items;
};

// 自拟的一堆假参数：只用于离线工具与版式断言，游戏内使用光影包的真实参数。
SettingsModel makeDemoSettings();

// 把真实光影包的参数映射成设置模型。locale 是游戏当前语言（如 en_US / zh_CN），
// 用来挑包内 lang/<locale>.lang；留空则按内置回退顺序。
SettingsModel settingsFromPack(ShaderPack const& pack, std::string const& locale = {});

// ---------------------------------------------------------------- 版式

struct SettingBox {
    int  index  = -1; // SettingsModel.items 下标
    int  row    = -1; // 第几个可视行（分类 / 整行 / 半行各占一行）
    int  column = 0;  // 0 = 左, 1 = 右, -1 = 整行
    Rect row_;        // 该项在界面上的矩形（半行就是它那一半）
    Rect label;       // 名字区
    Rect control;     // 可交互控件区
    bool expanded = false;
    bool button   = false; // 顶层分类按钮：整格可点，只画名字（打开下一层）
};

struct SettingsLayout {
    Rect content;                // 中间内容栏（外框）
    Rect configBar;              // 顶部分隔线下的「配置」栏
    Rect configButton;           // 当前预设名 + 展开箭头
    Rect viewport;               // 可滚动的可视区
    Rect headerBack;             // 左上角「返回」
    Rect header[4];              // 右上角 保存 / 导入 / 导出 / 重置
    int  headerCount = 4;
    Rect scrollTrack;            // 右侧滚动条轨道
    Rect scrollHandle;           // 滚动条滑块
    std::vector<SettingBox> slots;  // 全部设置项（含不可见），键盘导航用
    std::vector<int>        visible; // slots 里当前完全落在可视区内的下标
    float contentHeight = 0;
    float scroll        = 0;
    float maxScroll     = 0;
};

// view = 当前这一页要显示的项（SettingsModel.items 的下标，按显示顺序）。
// hovered = 鼠标悬停项：**滑块只要鼠标移上去就展开**（占满它所在的那个文本框），
// 鼠标一离开立刻复原（对齐 Iris：光标停在值框上就变成可拖的滑块）。
SettingsLayout buildSettingsLayout(float width, float height, SettingsModel const& model,
                                   std::vector<int> const& view, SettingPageStyle style, int hovered, float scroll);

// 顶层（分类按钮）与某一层（该分类下的参数）的视图。
std::vector<int> settingsTopLevelView(SettingsModel const& model);
std::vector<int> settingsCategoryView(SettingsModel const& model, int categoryIndex);
// 包里只有参数、没有 screen.* 分类时的“整页参数”视图（所有非分类项，按声明顺序）。
std::vector<int> settingsFlatView(SettingsModel const& model);
// 某个分类用来显示的文案（i18n key）。
std::string settingsCategoryLabel(SettingsModel const& model, int categoryIndex);

enum class SettingPart { None, Label, Control };

// 命中测试：返回 SettingsModel.items 下标，-1 = 没命中。
int settingsHitTest(SettingsLayout const& L, float x, float y, SettingPart* part = nullptr);

// ---------------------------------------------------------------- 值操作

[[nodiscard]] bool        settingsIsOn(SettingItem const& item);
[[nodiscard]] std::string settingsEnumLabel(SettingItem const& item);    // 当前枚举值的 label（i18n key）
[[nodiscard]] std::string settingsFormatNumber(SettingItem const& item); // 滑块当前值文本
[[nodiscard]] std::string settingsValueText(SettingItem const& item, std::string const& on,
                                            std::string const& off);
// 滑块的归一化位置（0..1）：滑标 / 名字框 / 数值都用它定位。
[[nodiscard]] float settingsSliderFraction(SettingItem const& item);

// delta > 0 增、< 0 减：bool 取反、enum 循环、color 换色、slider 加一档。
void settingsAdjust(SettingsModel& model, int index, int delta);
// 按拖动位置设置滑块值：t 是 [0,1] 的比例。
void settingsSetSlider(SettingsModel& model, int index, float t);
// 重置为默认配置（bool/enum/color 回到 defValue，slider 回到 def）。
void settingsResetDefaults(SettingsModel& model);

// 键盘导航：在可视行之间移动选择（跳过分类）；dx = ±1 左右、dy = ±1 上下。
int settingsMove(SettingsLayout const& L, int current, int dx, int dy);

// 视图里第一个可选中的项；没有返回 -1。
int settingsFirstSelectable(SettingsModel const& model, std::vector<int> const& view);

} // namespace renderdragon_shader_loader::ui
