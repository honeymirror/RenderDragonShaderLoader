#pragma once

#include <chrono>
#include <memory>
#include <mutex>

#include "ui/UiScreen.h"

class MinecraftUIRenderContext;
class IClientInstance;
class AbstractScene;
class ScreenView;

namespace renderdragon_shader_loader {

// 游戏内对界面核心 ui::Screen（见 src/ui/UiScreen.*）的适配层：
//   - 推一个游戏原生空对话框当画布，拿到焦点/光标所有权（照 Lamium 的做法）；
//   - 把 MinecraftUIRenderContext 包成 ui::Canvas（矩形 + 文字）；
//   - 把 MaterialHooks / i18n / 光影包数据接成 ui::ScreenHost；
//   - 每帧喂指针位置，转发键盘/鼠标事件。
// 布局、交互、绘制全部在 ui::Screen 里，游戏侧只负责"喂数据 + 转发事件"。
class ShaderManagerScreen {
public:
    static ShaderManagerScreen& getInstance();

    [[nodiscard]] bool visible() const;
    // 设置页正在"录键位"：宿主的热键监听要放行这一次按键，交给界面处理。
    [[nodiscard]] bool capturingKey() const;
    void               toggle();
    void               close();
    void               open();
    void               open(IClientInstance& client);
    // 外部拖放导入完成后回调：面板开着就立刻重扫并刷新列表。
    void               refreshPacks();

    // 启动时按配置自动恢复了上次的光影：把这个"已生效"的状态同步给界面。
    void               setAppliedShaderPack(std::string const& packName, bool enabled);

    // 只在自己占位屏幕的那一趟 UI pass 里调用。BeforeUIRenderEvent 会 cancel，
    // 原生命令对话框就不会画出来；界面直接画在它的位置上。
    void render(MinecraftUIRenderContext& context, float screenWidth, float screenHeight);

    // 每个 UI pass 结束后调用：释放上一帧退休的场景，并处理“push 被游戏丢掉”的兜底。
    void afterFrame(MinecraftUIRenderContext& context);

    // 每帧由渲染回调喂进来的权威指针位置（GUI 单位，来自 ScreenView::mPointerLocationPrevious）。
    // 鼠标事件里的坐标不可靠（实测是"最后一次移动的位置"），所以命中测试都用这个。
    void setPointer(float x, float y);

    // 背景压暗档位（0 = 完全透明但会露出占位对话框；1 = 全遮住）。
    // 对标 Lamium 那个 schematic.menuBackground 的 关/弱/强。
    void setBackdrop(float alpha);

    // 返回 true 表示该输入已被界面消费（调用方应 cancel 事件）。
    bool handleKey(int keyCode);
    // action/data 直接用 MouseAction 枚举；px/py 是鼠标事件的 GUI 坐标。
    // wheelDelta：ActionWheel 时的滚轮量（其它 action 忽略）。
    bool handleMouse(int action, float px, float py, bool down, int wheelDelta = 0);

    // 该 AbstractScene 是不是我们的占位屏幕（用于跳过它自带的原生绘制）。
    [[nodiscard]] bool isPlaceholderScene(AbstractScene const* scene) const;
    // UIScene::$onScreenExit 回调：占位屏幕被游戏弹出时同步清状态。
    void notifyPlaceholderScenePopped(AbstractScene const* scene);

private:
    ShaderManagerScreen();

    void               openOnClientThread();
    void               pushPlaceholderScene(IClientInstance& client); // 仅客户端线程调用
    void               popPlaceholderScene();
    void               popOnClientThread(std::shared_ptr<AbstractScene> scene);
    [[nodiscard]] bool ownsTopLocked() const;
    void               abandonSceneLocked();

    ui::Screen mScreen;

    // SceneStack、mScreen 和这些标志会被 UI 渲染回调、游戏输入回调和客户端线程执行器
    // 同时触碰；全部用同一把递归锁收口。所有 origin()/游戏 API 调用都在锁外进行。
    mutable std::recursive_mutex mMutex;

    IClientInstance* mClient = nullptr;
    // 占位屏幕：让游戏进入"屏幕模式"（自带输入隔离 + 光标 + HUD 隐藏）。
    std::shared_ptr<AbstractScene> mScene;
    // 被游戏弹出后先留一帧再释放：栈可能在本回调返回后还引用它。
    std::shared_ptr<AbstractScene> mRetiredScene;
    // 整屏压暗档位（config 的 uiBackdrop）。原生对话框已由 BeforeUIRenderEvent
    // cancel 掉，所以这里只控制世界透出多少。
    float mBackdrop = 0.f;
    // 开/关请求统一排到客户端线程；栈状态只在客户端线程改。
    bool mOpenRequested = false;
    bool mOpenScheduled = false;
    // schedulePopScreen 已经排队后不再重复排；等游戏的 onScreenExit 回来再清。
    bool mWantPopScene = false;
    int  mPopRetries   = 0;
    // 占位屏幕 push 的时刻：只用于检测 push 被游戏丢弃。
    std::chrono::steady_clock::time_point mScenePushedAt{};
    // 上一帧"游戏窗口是否在前台"：从其它窗口切回来时重扫一次包目录（玩家可能刚丢进新 zip）。
    bool mWasFocused = false;
};

// 占位屏幕的生命周期 hook：记录自己那一趟 UI pass、让世界画在背后、
// 并在屏幕退出时同步回收状态。在 enable/disable 里调用。
void installNativeUiSuppressHooks();
void uninstallNativeUiSuppressHooks();

// 这一趟 UI pass 是不是"我们占位屏幕"那一趟（见 ShaderManagerScreen::render 的 ourPass）。
bool isOurUiPass(ScreenView const& view);


// 卡死诊断看门狗（独立线程）：渲染回调停摆 5 秒会写 data/ui-hang.log。
// （以前的 F9"强制关面板"逃生热键已删除。）enable()/disable() 里开关。
void startHangWatchdog();
void stopHangWatchdog();

} // namespace renderdragon_shader_loader
