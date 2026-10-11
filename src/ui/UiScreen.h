#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "core/ShaderPack.h"
#include "ui/UiCanvas.h"
#include "ui/UiLayout.h"
#include "ui/UiSettings.h"

// 界面的「核心」：状态机 + 版式 + 交互 + 绘制指令，全部不依赖游戏类型，
// 因此可以被不同宿主共用：
//   - 游戏内：ShaderManagerScreen（占位屏幕 / 事件 / hook / 材质热重载）；
//   - 其他宿主只要实现 ui::Canvas 与 ScreenHost 就能复用同一份界面代码。
// 数据源是 core/ShaderPack 的 ShaderPackStore（同样不依赖游戏）。
namespace renderdragon_shader_loader::ui {

// UI 需要外部世界提供的能力，由宿主注入。
struct ScreenHost {
    std::function<std::string(std::string_view)>      translate;      // 文案（i18n）
    std::function<void(std::string const&)>           log;            // 诊断日志（可空）
    std::function<void(ShaderPack const&)>            reload;         // 热重载材质
    std::function<void()>                             restoreVanilla; // 关掉光影：回到原版材质
    std::function<void()>                             openFolder;     // 打开光影包目录
    std::function<void()>                             rescanPacks;    // 重新扫描光影包目录
    // 设置页：导入 / 导出配置文件（由宿主弹系统文件对话框）。
    // suggestedName = 当前光影包的建议文件名（<包名>.txt）；导入时把读到的内容写进 outText。
    // 返回 false = 用户取消或读写失败。
    std::function<bool(std::string const& suggestedName, std::string& outText)>      importSettings;
    std::function<bool(std::string const& suggestedName, std::string const& text)>   exportSettings;
    std::function<void()>                             playClick;      // 按钮/调参数时的 UI 点击音效
    std::function<void()>                             closeScreen;    // 关闭界面（游戏侧弹占位屏幕）
};

enum class MouseButton { Left = 1, Right = 2 };

class Screen {
public:
    // List = 光影包列表；Settings = Iris 风格的自定义参数设置界面。
    enum class Mode { List, Settings };

    void setHost(ScreenHost host) { mHost = std::move(host); }

    [[nodiscard]] bool visible() const { return mVisible; }
    void               setVisible(bool value);
    void               open();  // 显示并回到列表页（重新记录选项快照）
    void               close(); // 隐藏并通知宿主
    // 宿主侧的占位屏幕已经由游戏自己弹出：只隐藏并丢弃暂存，不再回调 closeScreen，
    // 否则会再排一次 pop，把游戏自己的下一层屏幕弹掉。
    void dismiss();
    // 重新扫描包目录，并按包名把当前选中项找回来（新丢进来的 zip 会出现在列表里）。
    void refreshPacks();

    // 引擎维护的指针位置（GUI 单位），用于绘制悬停效果。
    // 点击命中用 handleMouseButton 传入的事件坐标，避免拿上一帧位置点错区域。
    void setPointer(float x, float y);

    // 绘制一帧。width/height 是 GUI 单位下的屏幕尺寸。
    void render(Canvas& canvas, float width, float height);

    // 输入。坐标必须是 GUI 单位（游戏侧负责用 mInvGuiScale 换算）。
    bool handleKey(int keyCode); // Windows 虚拟键码
    // 滚轮：正值向上、负值向下（游戏里 ActionWheel 的 buttonData）。
    bool handleWheel(int delta);
    void handleMouseMove(float x, float y, bool down);
    // x/y 是本次事件的 GUI 单位坐标，点击命中的权威坐标。
    bool handleMouseButton(MouseButton button, float x, float y, bool down);

    // 编译进行中：整个界面锁死，宿主应吞掉输入。
    [[nodiscard]] bool blocked() const;
    // 「完成」在编译结束后的排队动作还没处理完。
    [[nodiscard]] bool waitingToReturn() const { return mPending != Pending::None; }
    // 透明预览模式：只画右下角按钮，其余内容全隐藏。
    [[nodiscard]] bool transparentPreview() const { return mTransparentPreview; }

    [[nodiscard]] Layout const& layout() const { return mLayout; }
    [[nodiscard]] SettingsLayout const& settingsLayout() const { return mSettingsLayout; }
    [[nodiscard]] SettingsModel const&   settingsModel() const { return mSettings; }
    [[nodiscard]] Mode          mode() const { return mMode; }

private:
    std::string tr(std::string_view key) const;
    void        log(std::string const& line) const;
    // Iris 同款：改参数 / 点按钮时播一次原版界面的点击音效。
    void        click() const;
    // ---- 底部状态提示（编译状态 / 我们自己的提示）的淡出 ----
    // 文字一变就重新计时：显示满 3 秒后开始淡出，第 4 秒整消失；底色是透明的。
    [[nodiscard]] std::string effectiveStatus() const;
    [[nodiscard]] float       statusAlpha(std::string const& message) const;
    void                      trackStatus(std::string const& message);

    void drawHeader(Canvas& canvas, float width, float y, std::string const& title, std::string const& subtitle);
    void drawContentBackdrop(Canvas& canvas, float width);
    void drawBottomBar(Canvas& canvas, float width, float height);
    void drawButton(Canvas& canvas, Rect const& r, std::string const& value, int index);
    void drawPreviewButton(Canvas& canvas);
    // Iris 风格右侧滚动条：贴屏幕最右缘，暗色轨道 + 浅灰滑块。
    void drawScrollBar(Canvas& canvas, Rect const& track, Rect const& handle);
    // 列表/参数页底部那行状态文字（编译进度、我们的提示等）。
    void drawStatusLine(Canvas& canvas, float width, float y) const;
    void drawList(Canvas& canvas, float width);
    void drawSettings(Canvas& canvas, float width, float height);

    void              activateRow(int index);
    void              activateButton(int index);
    void              adjustSelected(int delta);
    void              selectRow(int delta);
    [[nodiscard]] int contentCount() const;
    // 列表页：第 0 项是"光影：已启用/已禁用"开关，后面才是各个包。
    [[nodiscard]] int  listFirstVisible() const;
    [[nodiscard]] int  listIndexAtRow(int row) const;
    [[nodiscard]] int  listMaxFirst() const;   // 列表能滚到的最靠后的"第一个可见包"
    void               listSyncToRow();        // 让选中行重新落进可视窗口（键盘移动后用）
    // 列表页滚动条的轨道 / 滑块（内容不溢出时都是空 rect）；绘制与拖动共用同一套几何。
    void               listScrollBar(float width, Rect& track, Rect& handle) const;
    void               updateScrollDrags(float y); // 按住滚动条拖动时按指针位置换算滚动量
    void               toggleShaders();
    void               snapshotParameters();
    [[nodiscard]] bool pendingChanged() const;
    bool               applyPending();
    void               discardPending(bool refreshPacks = true);
    void               notice(std::string text, bool failed);
    [[nodiscard]] bool dirty() const;

    // ---- 设置页（Iris 风格自定义参数界面）----
    void enterSettings();                        // 进入设置页（载入模型 + 回到第一项）
    void openSettingsCategory(int categoryIndex); // 打开某个分类（下一层）
    void settingsGoBack();                        // 返回上一层；已在顶层就回列表页
    void leaveSettings();                        // 返回列表页
    bool handleSettingsKey(int keyCode);         // 设置页键盘；返回是否已处理
    void moveSettingSelection(int dx, int dy);   // 方向键移动选择（跳过分类）
    void clampSettingScroll();                   // 保证选中项可见
    void updateSettingDrag(float x, float y);    // 拖动滑块：按指针 x 落到对应数值
    void applySettingsEdits();                   // 「应用」：记下快照（编译暂停，只存盘/占位）
    void discardSettingsEdits();                 // 「取消」：回到进设置页时的快照
    [[nodiscard]] bool settingsDirty() const;
    // 顶栏「导入 / 导出」：宿主负责弹文件对话框，这里只做序列化与回填。
    void importSettingsFile();
    void exportSettingsFile();
    [[nodiscard]] std::string suggestedSettingsFileName() const; // <包名>.txt
    int                       applySettingsText(std::string const& text); // 返回成功应用的条数

    bool  mVisible     = false;
    Mode  mMode        = Mode::List;
    int   mPack        = 0;
    int   mRow         = 0;  // List=包下标
    int   mButtonFocus = -1; // -1 = 焦点在内容区
    int   mHeaderFocus = -1; // 设置页顶栏按钮焦点（0=返回 1..3=导入/导出/重置）
    float mMouseX      = -1.f;
    float mMouseY      = -1.f;
    bool  mMouseDown   = false;
    // 最近一帧的屏幕尺寸（GUI 单位）：进设置页时要立刻算出一份可用版式，
    // 否则"进入设置页"到"下一帧渲染"之间的第一次点击会命中空版式。
    float mLastWidth  = 0.f;
    float mLastHeight = 0.f;
    // 右下角按钮切换“只看画面”模式：全透明，只保留按钮本身。
    bool  mTransparentPreview = false;

    Layout     mLayout;
    ScreenHost mHost;

    SettingsModel   mSettings;        // 当前设置页模型（本期 = makeDemoSettings 的假参数）
    SettingsLayout  mSettingsLayout;  // 上一帧的设置页版式（命中测试用）
    std::vector<int> mSettingsView;   // 当前这一页显示的项（模型下标）
    // -1 = 顶层分类页（按钮网格）；-2 = 包里没有分类时的"整页参数"；>= 0 = 某个分类的模型下标
    int             mSettingsPage = -1;
    int             mSettingsHover = -1; // 本帧鼠标悬停的项（滑块靠它展开）
    int             mSettingIndex = -1;
    float           mSettingScroll = 0.f;
    bool            mSettingDragging = false;
    // 滚动条按住拖动：mScrollGrabDy = 按下点相对滑块顶端的偏移，保证拖起来不跳。
    bool            mSettingScrollDragging = false;
    float           mScrollGrabDy          = 0.f;
    bool            mListScrollDragging    = false;
    float           mListGrabDy            = 0.f;
    // 列表页的滚动位置：第一个可见包的下标。滚轮只动它，**不再连带改选中行**。
    int             mListFirst = 0;
    std::string     mSettingsSnapshot; // 进设置页时的值快照：判断有没有改动 / 取消时回退

    enum class Pending { None, BackToList };
    Pending mPending = Pending::None;

    // 打开界面时记录的选项快照，用来判断「完成」时要不要编译。
    std::string mSnapshotPack;
    std::string mSnapshot;

    // 界面上的一切都只是"暂存"（mEnabled / mPack / 参数），只有「应用/完成」才落地；
    // 下面两个是**当前已经生效**的状态，用来判断"有没有改动"和「取消」时回退。
    // 默认关闭：进游戏就是原版材质，要玩家自己选一个光影再用（对齐 Iris 的初始态）。
    bool mEnabled        = false; // 暂存：界面上的开关显示
    bool mEnabledApplied = false; // 已生效：光影此刻是否真的开着
    int  mAppliedPack    = -1;    // 已生效：此刻挂着的包（-1 = 原版材质）

    // 界面自己的提示（比如"这个包还没有编译产物"）：非空时优先于宿主的编译状态显示。
    std::string mNotice;
    bool        mNoticeFailed = false;
    // 状态提示的淡出计时：mStatusShownAt 是"当前这条文字"第一次出现的时刻。
    std::string                            mStatusShown;
    std::chrono::steady_clock::time_point  mStatusShownAt{};
};

} // namespace renderdragon_shader_loader::ui
