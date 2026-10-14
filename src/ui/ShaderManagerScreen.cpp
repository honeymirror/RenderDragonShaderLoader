#include "ui/ShaderManagerScreen.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// clang-format off
#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>
#include <TlHelp32.h>
#endif
// clang-format on

#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/Font.h"
#include "mc/client/gui/FontHandle.h"
#include "mc/client/gui/FontRepository.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/gui/TextAlignment.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/screens/AbstractScene.h"
#include "mc/client/gui/screens/SceneFactory.h"
#include "mc/client/gui/screens/SceneStack.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/gui/screens/UIScene.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/client/sound/SoundEngine.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/deps/input/RectangleArea.h"

#include "core/GraphicsModeController.h"
#include "core/Hotkey.h"
#include "core/ShaderPack.h"
#include "core/VibrantVisuals.h"
#include "hook/MaterialHooks.h"
#include "mod/Config.h"
#include "mod/RenderDragonShaderLoader.h"

namespace renderdragon_shader_loader {
namespace {

void uiTrace(std::string const& line); // 定义在下面

Font& fontOf(MinecraftUIRenderContext& context) {
    auto repo = context.mClient.getMinecraftGame_DEPRECATED().getFontRepository();

    // 一次性把游戏里可用的字体别名写进 ui-trace.log。以后要挑"更像 Java 版 Iris 的字体"时
    // 直接照这份名单换下面 getFontFromFontType(...) 里的名字，不用瞎猜。
    static bool dumpedFonts = false;
    if (!dumpedFonts) {
        dumpedFonts = true;
        std::string names;
        for (auto const& entry : *repo->mFontNameToIdentifier) {
            if (!names.empty()) names += ", ";
            names += entry.first;
        }
        uiTrace("fonts available: " + names);
    }

    return repo->getFontFromFontType("default").getFont();
}

mce::Color toMc(ui::Color const& color) { return mce::Color{color.r, color.g, color.b, color.a}; }

::ui::TextAlignment toMc(ui::Align align) {
    switch (align) {
    case ui::Align::Center:
        return ::ui::TextAlignment::Center;
    case ui::Align::Right:
        return ::ui::TextAlignment::Right;
    case ui::Align::Left:
    default:
        return ::ui::TextAlignment::Left;
    }
}

void drawLabel(
    MinecraftUIRenderContext& context,
    Font&                     font,
    float                     x,
    float                     y,
    float                     width,
    std::string               text,
    mce::Color                color,
    ::ui::TextAlignment       align,
    float                     size
) {
    TextMeasureData const  measure{size, 0.f, false, false, false, align};
    CaretMeasureData const caret{-1, false};
    context.drawText(
        font,
        RectangleArea{x, x + width, y, y + 14.f * size},
        std::move(text),
        color,
        size,
        align,
        measure,
        caret
    );
}

// 把 MinecraftUIRenderContext 包成 ui::Canvas：界面核心只认这个接口，
// 游戏里这么实现；任何别的宿主只要实现 ui::Canvas 也能复用同一份界面代码。
//
// 性能/稳定性要点：**绝不能每画一个矩形就 flushImages 一次**。
// 之前 `fillRect()` 是 fillRectangle + flushImages 逐条提交，我们一帧要画 30 多个矩形，
// 而 AfterUIRenderEvent 每帧会被触发 ~8 次（实测 ~460 次/秒）⇒ 每秒上万次批次提交，
// 这是面板开着时随机整机卡死最可疑的元凶之一。
// 现在改成：fill/text 只入队，flush() 时一次性提交（所有填充 → flushImages → 所有文字 → flushText）。
// 我们的界面里文字永远压在填充之上，所以这种两段式提交在视觉上等价。
class GameCanvas final : public ui::Canvas {
public:
    explicit GameCanvas(MinecraftUIRenderContext& context) : mContext(context), mFont(fontOf(context)) {}

    void fill(float x, float y, float w, float h, ui::Color color, float alpha) override {
        if (w <= 0.f || h <= 0.f) return;
        mFills.push_back({
            RectangleArea{x, x + w, y, y + h},
            toMc(color),
            alpha
        });
    }

    void text(
        float              x,
        float              y,
        float              boxWidth,
        std::string const& value,
        ui::Color          color,
        ui::Align          align,
        float              size
    ) override {
        if (value.empty() || boxWidth <= 0.f) return;
        mTexts.push_back({x, y, boxWidth, value, toMc(color), toMc(align), size});
    }

    // 量文字宽度：给"跟着鼠标走的滑块名字浮层"定宽用（见 UiScreen::drawSettings）。
    [[nodiscard]] float textWidth(std::string const& value, float size) const override {
        if (value.empty()) return 0.f;
        return (float)mFont.getLineLength(value, size, false);
    }

    void flush() override {
        if (!mFills.empty()) {
            for (auto const& f : mFills) mContext.fillRectangle(f.rect, f.color, f.alpha);
            mFills.clear();
            mContext.flushImages(mce::Color{1.f, 1.f, 1.f, 1.f}, 1.f, HashedString{"ui_fillColor"});
        }
        if (!mTexts.empty()) {
            for (auto const& t : mTexts) {
                drawLabel(mContext, mFont, t.x, t.y, t.boxWidth, t.value, t.color, t.align, t.size);
            }
            mTexts.clear();
            mContext.flushText(0.f, std::nullopt);
        }
    }

private:
    struct Fill {
        RectangleArea rect;
        mce::Color    color;
        float         alpha = 1.f;
    };
    struct Text {
        float               x        = 0.f;
        float               y        = 0.f;
        float               boxWidth = 0.f;
        std::string         value;
        mce::Color          color;
        ::ui::TextAlignment align = ::ui::TextAlignment::Left;
        float               size  = 1.f;
    };

    MinecraftUIRenderContext& mContext;
    Font&                     mFont;
    std::vector<Fill>         mFills;
    std::vector<Text>         mTexts;
};

std::string tr(std::string_view key) {
    auto value = ll::i18n::getInstance().get(key, {});
    return value.empty() ? std::string(key) : std::string(value);
}

// 诊断日志：直接写文件并**立即 flush**，不走游戏 logger。
// 游戏卡死时它的 logger 可能把最后几行留在缓冲区里丢掉（实测：末行被截断、后面发生
// 过什么完全看不到）。这个文件写得慢一点没关系，关键是卡死时能知道最后走到哪一步。
void uiTrace(std::string const& line) {
    static std::mutex                  mutex;
    std::lock_guard                    lock(mutex);
    static std::filesystem::path const path = [] {
        auto            dir = ll::mod::NativeMod::current()->getModDir() / "data";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto file = dir / "ui-trace.log";
        // 每个进程只保留最近一次运行：文件太大就清掉，免得越滚越大。
        std::error_code sizeEc;
        if (std::filesystem::file_size(file, sizeEc) > 8 * 1024 * 1024) {
            std::filesystem::remove(file, sizeEc);
        }
        return file;
    }();
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    static auto const start = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    out << "[" << ms << "ms] " << line << "\n";
}

// ---------------------------------------------------------------- 卡死诊断看门狗
//
// 面板开着时游戏会"卡住"（音频还在响、按 Esc 没反应）。要区分两种情况：
//   - 我们的渲染回调停了（游戏主线程卡在别处）；
//   - 整个进程死了。
// 这里用**独立线程**做一件事：渲染回调超过 5 秒没来 → 抓一份报告，把每个线程栈上
// "看着像代码地址"的值换算成模块+偏移，写进 data/ui-hang.log（没有符号也能看出卡在
// 哪个模块/哪个偏移，再和我们 hook 的目标地址对照就能定位）。
// （F9「强制关面板」逃生热键已按用户要求删除。）
std::atomic<long long> gLastRenderTickMs{0};
std::atomic<bool>      gWatchdogStop{false};
std::thread            gWatchdogThread;
// 游戏侧"调用 UIScene::render 的那条指令的下一条"地址（真实游戏地址，可以用来和卡死栈对照）。
std::atomic<uintptr_t> gRenderCallSite{0};

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct ModuleRange {
    uintptr_t   base = 0;
    uintptr_t   end  = 0;
    std::string name;
};

std::string narrow(std::wstring const& w) {
    if (w.empty()) return {};
    int         n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::vector<ModuleRange> listModules() {
    std::vector<ModuleRange> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            uintptr_t const base = (uintptr_t)me.modBaseAddr;
            out.push_back({base, base + me.modBaseSize, narrow(me.szModule)});
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    std::sort(out.begin(), out.end(), [](ModuleRange const& a, ModuleRange const& b) { return a.base < b.base; });
    return out;
}

std::string describeAddress(uintptr_t addr, std::vector<ModuleRange> const& mods) {
    char buf[320];
    for (auto const& m : mods) {
        if (addr >= m.base && addr < m.end) {
            std::snprintf(buf, sizeof(buf), "%s+0x%llX", m.name.c_str(), (unsigned long long)(addr - m.base));
            return buf;
        }
    }
    std::snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)addr);
    return buf;
}

// 扫描每个线程栈顶若干字节里"落在某个模块地址范围内"的值 —— 近似调用栈。
std::string captureThreadStacks(std::vector<ModuleRange> const& mods) {
    std::string out;
    HANDLE      snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE th = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE,
                te.th32ThreadID
            );
            if (th == nullptr) continue;
            if (SuspendThread(th) == (DWORD)-1) {
                CloseHandle(th);
                continue;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th, &ctx)) {
                uintptr_t const sp = (uintptr_t)ctx.Rsp;
                out += "  thread " + std::to_string(te.th32ThreadID) + " rsp=" + describeAddress(sp, mods) + "\n";
                std::vector<unsigned char> buf(8192, 0);
                SIZE_T                     got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)sp, buf.data(), buf.size(), &got)) {
                    int printed = 0;
                    for (size_t off = 0; off + 8 <= got && printed < 20; off += 8) {
                        uintptr_t v = 0;
                        std::memcpy(&v, buf.data() + off, 8);
                        for (auto const& m : mods) {
                            if (v >= m.base && v < m.end) {
                                out += "        +" + std::to_string(off) + "  " + describeAddress(v, mods) + "\n";
                                ++printed;
                                break;
                            }
                        }
                    }
                }
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return out;
}

void writeHangReport(std::string const& reason) {
    auto const  mods = listModules();
    std::string text = "==== HANG REPORT (" + reason + ") ====\n";

    // 参考地址：都是**真实地址**（游戏侧的调用点 / 我们自己的 detour，后者可用随模组发布的
    // RenderDragonShaderLoader.pdb 解析出函数名）。报告里的模块+偏移可以和这些对照。
    text += "reference addresses:\n";
    text += "  game: UIScene::render called from = " + describeAddress(gRenderCallSite.load(), mods) + "\n";
    // 我们自己的函数：地址可用随模组发布的 RenderDragonShaderLoader.pdb 解析出符号名。
    text += "  ours: isOurUiPass                 = " + describeAddress((uintptr_t)&isOurUiPass, mods) + "\n";
    text += "  ours: startHangWatchdog           = " + describeAddress((uintptr_t)&startHangWatchdog, mods) + "\n";
    text += "threads:\n" + captureThreadStacks(mods) + "\n";

    auto            dir = ll::mod::NativeMod::current()->getModDir() / "data";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / "ui-hang.log", std::ios::app);
    if (out) out << text << std::flush;
    ll::mod::NativeMod::current()->getLogger().warn("[ui] hang report written ({} bytes)", text.size());
}

void watchdogMain() {
    while (!gWatchdogStop.load()) {
        long long const last = gLastRenderTickMs.load();
        if (last != 0 && nowMs() - last > 5000 && ShaderManagerScreen::getInstance().visible()) {
            writeHangReport("render callback silent for " + std::to_string(nowMs() - last) + " ms");
            gLastRenderTickMs.store(nowMs()); // 报一次就重置，别刷屏
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

} // namespace

// 这两个是给模组 enable/disable 调用的，必须放匿名命名空间外面（外部链接）。
void startHangWatchdog() {
    if (gWatchdogThread.joinable()) return;
    gWatchdogStop   = false;
    gWatchdogThread = std::thread(watchdogMain);
}

void stopHangWatchdog() {
    gWatchdogStop = true;
    if (gWatchdogThread.joinable()) gWatchdogThread.join();
}

// 注意：这里【不能】跳过 UIScene::$render。早先我写过 `if (是我们的屏幕) return;`，
// 结果屏幕没被真正激活——游戏不交出鼠标（mouseGrabbed 一直是 true，看不到光标、
// 鼠标也不能动）。Lamium 的同类 hook 也是照常调用 origin()。
// 原生对话框会照常画出来，我们的界面由 AfterUIRenderEvent 之后绘制、盖在上面。

// （原来这里有一个 renderGameBehind hook：占位屏幕要让世界画在背后。现在不再使用任何
//   游戏屏幕，这段随之删除。）

// ---------------------------------------------------------------- 宿主装配

namespace {

std::wstring utf8ToWide(std::string const& text) {
    if (text.empty()) return {};
    int const n = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((std::size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), n);
    return out;
}

// 顶栏「导入 / 导出」用的系统文件对话框（comdlg32）。
// 和 Iris 一样是模态的：会占用游戏主线程直到玩家选完文件（期间游戏只是不渲染新帧）。
std::filesystem::path pickSettingsFile(
    std::filesystem::path const& initialDir,
    std::string const&           suggestedName,
    bool                         forWrite
) {
    wchar_t buffer[1024]{};
    if (!suggestedName.empty()) {
        std::wstring const wide = utf8ToWide(suggestedName);
        wcsncpy_s(buffer, std::size(buffer), wide.c_str(), _TRUNCATE);
    }
    std::wstring const dir = initialDir.wstring();

    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = ::GetForegroundWindow();
    ofn.lpstrFilter     = L"配置文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile       = buffer;
    ofn.nMaxFile        = (DWORD)std::size(buffer);
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt     = L"txt";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY |
                (forWrite ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));

    BOOL const ok = forWrite ? ::GetSaveFileNameW(&ofn) : ::GetOpenFileNameW(&ofn);
    if (!ok) return {};
    return std::filesystem::path(buffer);
}

std::string readTextFile(std::filesystem::path const& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeTextFile(std::filesystem::path const& file, std::string const& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), (std::streamsize)text.size());
    return out.good();
}

} // namespace

ShaderManagerScreen::ShaderManagerScreen() {
    ui::ScreenHost host;

    host.translate = [](std::string_view key) { return tr(key); };

    host.log = [](std::string const& line) {
        uiTrace(line); // 界面动作全部留一份"立即落盘"的副本，卡死时才能定位
        ll::mod::NativeMod::current()->getLogger().info("{}", line);
    };

    // 列表页的「应用」= 先按光影的渲染路线切图形模式，再把已有产物挂上去。
    host.reload = [](ShaderPack const& pack) {
        // 玩家可以在模组设置页里手动覆盖图形模式（自动判定只是按文件名猜的）。
        auto const overrideMode = RenderDragonShaderLoader::getInstance().getConfig().graphicsModeOverride;
        auto const mode         = applyRenderModeOverride(pack.renderMode, overrideMode);
        ll::thread::ClientThreadExecutor::getDefault().execute([pack, mode] {
            GraphicsModeController::applyForRenderMode(mode);
            MaterialHooks::getInstance().queueReload(pack.binsDir);
        });
    };

    // 「光影：已禁用」= 停止重定向材质文件，回到原版（Iris 的 Shaders: Disabled）。
    host.restoreVanilla = []() {
        ll::thread::ClientThreadExecutor::getDefault().execute([] {
            GraphicsModeController::applyForward();
            MaterialHooks::getInstance().restoreVanilla();
        });
    };

    host.openFolder = []() {
#ifdef _WIN32
        uiTrace("openFolder: begin");
        auto            dir = ll::mod::NativeMod::current()->getModDir() / "data" / "shaderpacks";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        // 用 explorer.exe 打开文件夹（避免额外链接 shell32）。
        std::wstring cmd = L"explorer.exe \"" + std::filesystem::absolute(dir, ec).wstring() + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        uiTrace("openFolder: done");
#endif
    };

    // 重新扫描包目录（玩家新丢进来的 zip 要能立刻出现在列表里）。
    host.rescanPacks = []() { RenderDragonShaderLoader::getInstance().rescan(); };

    // 设置页顶栏的导入 / 导出：弹系统文件对话框。
    //   - 导入：从**外部**挑一份配置读进来，同时把它复制进 data/shaderpacks/（模组的配置目录）。
    //   - 导出：把界面里当前的值写到玩家挑的位置，同时更新 data/shaderpacks/ 里那份。
    host.importSettings = [](std::string const& suggestedName, std::string& outText) -> bool {
        auto const dir = ll::mod::NativeMod::current()->getModDir() / "data" / "shaderpacks";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        auto const picked = pickSettingsFile(dir, suggestedName, false);
        if (picked.empty()) {
            uiTrace("settings: import cancelled");
            return false;
        }
        outText = readTextFile(picked);
        if (outText.empty()) {
            uiTrace("settings: import read failed -> " + picked.string());
            return false;
        }
        std::filesystem::copy_file(picked, dir / suggestedName, std::filesystem::copy_options::overwrite_existing, ec);
        uiTrace("settings: import " + picked.string() + " -> " + (dir / suggestedName).string());
        return true;
    };
    host.exportSettings = [](std::string const& suggestedName, std::string const& text) -> bool {
        auto const dir = ll::mod::NativeMod::current()->getModDir() / "data" / "shaderpacks";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        auto const picked = pickSettingsFile(dir, suggestedName, true);
        if (picked.empty()) {
            uiTrace("settings: export cancelled");
            return false;
        }
        if (!writeTextFile(picked, text)) {
            uiTrace("settings: export write failed -> " + picked.string());
            return false;
        }
        writeTextFile(dir / suggestedName, text); // shaderpacks 里也留一份（模组下次自己读）
        uiTrace("settings: export -> " + picked.string());
        return true;
    };

    // Iris 同款交互音：按钮 / 调参数时播一次原版界面的点击音
    // （Bedrock 的随机点击事件就是 UI 按钮用的那个 `random.click`，走 playUI 走 UI 音量）。
    host.playClick = []() {
        auto client = ll::service::getClientInstance();
        if (!client) return;
        auto sound = client->getSoundEngine(); // gsl::not_null<NonOwnerPointer<SoundEngine>>
        if (SoundEngine* raw = sound.operator->().get(); raw != nullptr) raw->playUI("random.click", 1.f, 1.f);
    };

    // 界面内部主动关闭（Esc / 取消 / 完成）时，先弹掉占位屏幕。
    host.closeScreen = []() {
        uiTrace("closeScreen: begin");
        ShaderManagerScreen::getInstance().popPlaceholderScene();
        uiTrace("closeScreen: done");
    };

    // ---- 模组设置页（底栏左下角「设置」）的数据通道 ----
    host.readSetting = [](std::string const& key) -> std::string {
        auto const& cfg = RenderDragonShaderLoader::getInstance().getConfig();
        if (key == "uiKey") return cfg.uiKey;
        if (key == "forceVibrantVisuals") return cfg.forceVibrantVisuals ? "ON" : "OFF";
        if (key == "autoLoadLastShader") return cfg.autoLoadLastShader ? "ON" : "OFF";
        if (key == "graphicsModeOverride") return std::to_string(cfg.graphicsModeOverride);
        return {};
    };
    host.writeSetting = [](std::string const& key, std::string const& value) -> bool {
        auto& mod = RenderDragonShaderLoader::getInstance();
        if (key == "uiKey") {
            bool const ok = mod.setPanelHotkey(value);
            uiTrace(std::string("settings: uiKey -> ") + value + (ok ? " ok" : " rejected"));
            return ok;
        }
        if (key == "forceVibrantVisuals") {
            bool const enabled = (value == "ON" || value == "TRUE" || value == "1");
            bool const ok      = mod.updateConfig([&](Config& cfg) { cfg.forceVibrantVisuals = enabled; });
            if (ok) setForceVibrantVisuals(enabled);
            uiTrace(std::string("settings: forceVibrantVisuals -> ") + (enabled ? "on" : "off"));
            return ok;
        }
        if (key == "autoLoadLastShader") {
            bool const enabled = (value == "ON" || value == "TRUE" || value == "1");
            bool const ok      = mod.updateConfig([&](Config& cfg) { cfg.autoLoadLastShader = enabled; });
            uiTrace(std::string("settings: autoLoadLastShader -> ") + (enabled ? "on" : "off"));
            return ok;
        }
        if (key == "graphicsModeOverride") {
            int const mode = std::clamp(std::atoi(value.c_str()), 0, 2);
            // 下一次加载光影 / 下次启动时生效；这里只落盘。
            bool const ok = mod.updateConfig([&](Config& cfg) { cfg.graphicsModeOverride = mode; });
            uiTrace("settings: graphicsModeOverride -> " + std::to_string(mode));
            return ok;
        }
        return false;
    };
    // 「应用 / 完成」真的把某个包挂上后：记住它，重启游戏时自动恢复。
    host.rememberSelection = [](std::string const& packName, bool enabled) {
        RenderDragonShaderLoader::getInstance().rememberShaderSelection(packName, enabled);
    };

    mScreen.setHost(std::move(host));
}

// ---------------------------------------------------------------- 占位屏幕状态
//
// 当前这一趟 UI pass 渲染的是不是我们的占位屏幕；是的话记下它的 ScreenView，
// BeforeUIRenderEvent 只接管这一趟，其余 HUD / 原版界面照常绘制。按线程存。
namespace {
thread_local ScreenView* gOurScreenView = nullptr;
} // namespace

ShaderManagerScreen& ShaderManagerScreen::getInstance() {
    static ShaderManagerScreen instance;
    return instance;
}

bool ShaderManagerScreen::visible() const {
    std::lock_guard lock(mMutex);
    return mScreen.visible();
}

bool ShaderManagerScreen::capturingKey() const {
    std::lock_guard lock(mMutex);
    return mScreen.capturingKey();
}

void ShaderManagerScreen::setAppliedShaderPack(std::string const& packName, bool enabled) {
    std::lock_guard lock(mMutex);
    mScreen.setAppliedShaderPack(packName, enabled);
}

void ShaderManagerScreen::setPointer(float x, float y) {
    std::lock_guard lock(mMutex);
    mScreen.setPointer(x, y);
}

void ShaderManagerScreen::setBackdrop(float alpha) {
    std::lock_guard lock(mMutex);
    mBackdrop = alpha;
}

void ShaderManagerScreen::refreshPacks() {
    std::lock_guard lock(mMutex);
    if (mScreen.visible()) mScreen.refreshPacks();
}

void ShaderManagerScreen::toggle() {
    std::lock_guard lock(mMutex);
    if (mScreen.visible()) {
        close();
    } else {
        open();
    }
}

void ShaderManagerScreen::open(IClientInstance& client) {
    {
        std::lock_guard lock(mMutex);
        mClient = &client;
    }
    open();
}

void ShaderManagerScreen::open() {
    bool schedule = false;
    {
        std::lock_guard lock(mMutex);
        if (mScreen.visible() || mOpenRequested || mOpenScheduled || mScene || mWantPopScene) {
            uiTrace("open: ignored (already opening/open)");
            return;
        }
        mOpenRequested = true;
        mOpenScheduled = true;
        schedule       = true;
    }

    if (schedule) {
        ll::thread::ClientThreadExecutor::getDefault().execute([this] { openOnClientThread(); });
    }
    uiTrace("open: scheduled on client thread");
}

void ShaderManagerScreen::close() {
    std::lock_guard lock(mMutex);
    uiTrace("close: begin");

    mOpenRequested = false;
    mOpenScheduled = false; // 已排队的 openOnClientThread 看到该标记后会直接退出

    if (!mScreen.visible() && !mScene) {
        uiTrace("close: no placeholder scene");
        return;
    }

    if (mScreen.visible()) {
        // Screen::close -> host.closeScreen -> popPlaceholderScene()。
        mScreen.setVisible(false);
    } else {
        popPlaceholderScene();
    }
    uiTrace("close: done");
}

void ShaderManagerScreen::openOnClientThread() {
    std::lock_guard lock(mMutex);
    mOpenScheduled = false;
    if (!mOpenRequested || mScreen.visible() || mScene || mWantPopScene) return;

    IClientInstance* client = mClient;
    if (client == nullptr) client = ll::service::getClientInstance().as_ptr();
    if (client == nullptr) {
        uiTrace("open: no client instance");
        mOpenRequested = false;
        return;
    }
    mClient = client;
    pushPlaceholderScene(*client);
    if (mScene == nullptr) mOpenRequested = false;
}

void ShaderManagerScreen::pushPlaceholderScene(IClientInstance& client) {
    uiTrace("pushScene: begin");
    auto scene = client.getSceneFactory().createCommonDialogInfoScreen("", "");
    if (!scene) {
        uiTrace("pushScene: createCommonDialogInfoScreen returned null");
        ll::mod::NativeMod::current()->getLogger().warn("[ui] createCommonDialogInfoScreen failed");
        return;
    }

    mScene         = std::move(scene);
    mScenePushedAt = std::chrono::steady_clock::now();
    // 先让界面可见，再 push：push 过程中若立刻触发 UI pass，Before hook 就会接管这一趟。
    mScreen.open();

    auto         sceneStack = client.getCurrentSceneStack();
    ISceneStack* stack      = sceneStack.operator->();
    if (stack == nullptr) {
        uiTrace("pushScene: no scene stack");
        abandonSceneLocked();
        return;
    }

    uiTrace("pushScene: pushScreen ...");
    stack->pushScreen(mScene, false);
    uiTrace("pushScene: pushScreen ok");
    ll::mod::NativeMod::current()->getLogger().info("[ui] pushed placeholder scene");
}

void ShaderManagerScreen::popPlaceholderScene() {
    std::shared_ptr<AbstractScene> scene;
    {
        std::lock_guard lock(mMutex);
        if (mWantPopScene) return;
        scene = mScene;
        if (!scene) {
            mOpenRequested = false;
            return;
        }
        mWantPopScene = true;
        mPopRetries   = 0;
    }

    uiTrace("popScene: scheduled on client thread");
    ll::thread::ClientThreadExecutor::getDefault().execute([this, scene] { popOnClientThread(scene); });
}

void ShaderManagerScreen::popOnClientThread(std::shared_ptr<AbstractScene> scene) {
    std::lock_guard lock(mMutex);
    if (!mWantPopScene || mScene != scene) return;
    if (mScreen.visible()) {
        // 关闭请求被 reopen 撤销了。
        mWantPopScene = false;
        return;
    }

    IClientInstance* client = mClient;
    if (client == nullptr) client = ll::service::getClientInstance().as_ptr();
    if (client == nullptr) {
        abandonSceneLocked();
        return;
    }

    auto         sceneStack = client->getCurrentSceneStack();
    ISceneStack* stack      = sceneStack.operator->();
    if (stack == nullptr) {
        abandonSceneLocked();
        return;
    }

    if (stack->getTopScene() != scene.get()) {
        // pushScreen 是队列式的；给它一点时间落到栈顶，绝不冒然 pop 游戏自己的屏幕。
        if (++mPopRetries <= 40) {
            ll::thread::ClientThreadExecutor::getDefault().executeAfter(
                [this, scene] { popOnClientThread(scene); },
                std::chrono::milliseconds(50)
            );
        } else {
            uiTrace("popScene: placeholder never reached top; clearing state");
            abandonSceneLocked();
        }
        return;
    }

    uiTrace("popScene: schedulePopScreen");
    stack->schedulePopScreen(1);
    mWantPopScene = false;
    mPopRetries   = 0;
    // mScene 继续保持到 onScreenExit 回调里回收，避免游戏栈在过渡期间还引用它。
}

bool ShaderManagerScreen::ownsTopLocked() const {
    if (mClient == nullptr || !mScene) return false;
    auto         sceneStack = mClient->getCurrentSceneStack();
    ISceneStack* stack      = sceneStack.operator->();
    return stack != nullptr && stack->getTopScene() == mScene.get();
}

void ShaderManagerScreen::abandonSceneLocked() {
    mRetiredScene  = std::move(mScene);
    mOpenRequested = false;
    mOpenScheduled = false;
    mWantPopScene  = false;
    mPopRetries    = 0;
    mScreen.dismiss();
}

bool ShaderManagerScreen::isPlaceholderScene(AbstractScene const* scene) const {
    std::lock_guard lock(mMutex);
    return scene != nullptr && mScene.get() == scene;
}

void ShaderManagerScreen::notifyPlaceholderScenePopped(AbstractScene const* scene) {
    std::lock_guard lock(mMutex);
    if (scene == nullptr || mScene.get() != scene) return;

    mRetiredScene  = std::move(mScene);
    mOpenRequested = false;
    mOpenScheduled = false;
    mWantPopScene  = false;
    mPopRetries    = 0;
    mScreen.dismiss();
    uiTrace("popScene: onScreenExit cleared placeholder state");
}

// ---------------------------------------------------------------- 渲染 / 输入

// ---------------------------------------------------------------- 占位屏幕 hook
//
// 不能跳过 UIScene::$render（那会让屏幕不激活、游戏不交出鼠标）。做法照 Lamium：
// 这个 hook 只记录"当前 UIScene 渲染的是我们的占位屏幕，它的 ScreenView 是哪个"；
// BeforeUIRenderEvent 收到匹配的 ScreenView 时 cancel + 自绘，原生命令对话框自然不会画。
LL_TYPE_INSTANCE_HOOK(
    PlaceholderSceneRender,
    ll::memory::HookPriority::Normal,
    UIScene,
    &UIScene::$render,
    void,
    ::ScreenContext&           screenContext,
    ::FrameRenderObject const& renderObj
) {
    bool const ours = ShaderManagerScreen::getInstance().isPlaceholderScene(this);
    gRenderCallSite.store((uintptr_t)_ReturnAddress()); // 记下游戏是从哪里调进 UIScene::render 的

    // 记住"这一趟 UI pass 渲染的是不是我们的占位屏幕"。
    ScreenView* const viewBefore = gOurScreenView;
    gOurScreenView               = ours ? mScreenView.get() : nullptr;
    origin(screenContext, renderObj);
    gOurScreenView = viewBefore;
}

// 我们那个占位屏幕只是"焦点/光标的所有者"，不需要视觉过渡动画。
// 开着过渡时这个原生对话框会进入"等动画"的状态（Lamium 对同一个对话框也是这么干的，
// 它的注释原话是 "Let it finish exiting without waiting for that animation"）。
// 我们的卡死时间点正好落在开屏后 0.4~1.5 秒的过渡窗口里，所以这条很关键。
LL_TYPE_INSTANCE_HOOK(
    PlaceholderSceneEntrance,
    ll::memory::HookPriority::Normal,
    UIScene,
    &UIScene::$onScreenEntrance,
    void,
    bool isRevisiting,
    bool doScreenTransitions
) {
    bool const ours = ShaderManagerScreen::getInstance().isPlaceholderScene(this);
    origin(isRevisiting, ours ? false : doScreenTransitions);
}

LL_TYPE_INSTANCE_HOOK(
    PlaceholderSceneExit,
    ll::memory::HookPriority::Normal,
    UIScene,
    &UIScene::$onScreenExit,
    void,
    bool                               isPopping,
    bool                               doScreenTransitions,
    ::std::shared_ptr<::AbstractScene> pushedScene
) {
    bool const ours = ShaderManagerScreen::getInstance().isPlaceholderScene(this);
    origin(isPopping, ours ? false : doScreenTransitions, std::move(pushedScene));
    if (ours && isPopping) {
        ShaderManagerScreen::getInstance().notifyPlaceholderScenePopped(this);
    }
}

// 占位屏幕要让世界画在背后（这样我们的半透明界面能透出世界）。
LL_TYPE_INSTANCE_HOOK(
    PlaceholderSceneWorldBehind,
    ll::memory::HookPriority::Normal,
    UIScene,
    &UIScene::$renderGameBehind,
    bool
) {
    if (ShaderManagerScreen::getInstance().isPlaceholderScene(this)) return true;
    return origin();
}

void installNativeUiSuppressHooks() {
    PlaceholderSceneRender::hook();
    PlaceholderSceneEntrance::hook();
    PlaceholderSceneExit::hook();
    PlaceholderSceneWorldBehind::hook();
    ll::mod::NativeMod::current()->getLogger().info("[ui] placeholder scene hooks installed");
}

void uninstallNativeUiSuppressHooks() {
    PlaceholderSceneWorldBehind::unhook();
    PlaceholderSceneExit::unhook();
    PlaceholderSceneEntrance::unhook();
    PlaceholderSceneRender::unhook();
}

void ShaderManagerScreen::render(MinecraftUIRenderContext& context, float screenWidth, float screenHeight) {
    std::lock_guard lock(mMutex);
    if (!mScreen.visible() || screenWidth <= 0.f || screenHeight <= 0.f) return;

    // 心跳诊断：每 ~5 秒一行，万一以后卡死，能看出游戏是哪一刻停的（不再逐帧写，太吵）。
    static unsigned long long sFrame = 0;
    ++sFrame;
    gLastRenderTickMs.store(nowMs()); // 看门狗用它判断"渲染回调是不是停了"

    if (sFrame % 300 == 0) {
        uiTrace("heartbeat frame=" + std::to_string(sFrame) + " vis=1");
    }

    // 「游戏窗口是不是前台窗口」：不是的话鼠标/键盘一律不喂给界面。
    // 之前用全局 GetAsyncKeyState，导致在资源管理器里点一下/打字也会把我们的 UI 点动。
    HWND const foreground    = GetForegroundWindow();
    DWORD      foregroundPid = 0;
    if (foreground != nullptr) GetWindowThreadProcessId(foreground, &foregroundPid);
    bool const appFocused = (foregroundPid != 0 && foregroundPid == GetCurrentProcessId());

    // 从别的窗口切回游戏且面板开着：玩家可能刚往 shaderpacks 里丢了新包，重扫一次。
    if (appFocused && !mWasFocused) mScreen.refreshPacks();
    mWasFocused = appFocused;

    if (mClient == nullptr) mClient = &context.mClient;

    GameCanvas canvas(context);

    // 整屏压暗档位来自 config 的 uiBackdrop。原生对话框已经由 BeforeUIRenderEvent
    // cancel 掉，这里只决定世界要透出多少。
    if (mBackdrop > 0.f && !mScreen.transparentPreview()) {
        canvas.fill(0.f, 0.f, screenWidth, screenHeight, ui::Color{0.05f, 0.055f, 0.06f, 1.f}, mBackdrop);
        canvas.flush();
    }

    mScreen.render(canvas, screenWidth, screenHeight);
}

bool isOurUiPass(ScreenView const& view) { return gOurScreenView != nullptr && gOurScreenView == &view; }

void ShaderManagerScreen::afterFrame(MinecraftUIRenderContext& context) {
    std::lock_guard lock(mMutex);

    // 上一帧退场的占位屏幕先留一帧再释放，避免游戏栈在回调返回后仍引用它。
    mRetiredScene.reset();
    gLastRenderTickMs.store(nowMs());

    if (!mScreen.visible()) return;
    if (mClient != nullptr && &context.mClient != mClient) return;

    if (mScene && !ownsTopLocked()) {
        if (std::chrono::steady_clock::now() - mScenePushedAt > std::chrono::seconds(3)) {
            uiTrace("placeholder scene was dropped by the game; clearing UI state");
            abandonSceneLocked();
        }
    }
}

bool ShaderManagerScreen::handleKey(int keyCode) {
    std::lock_guard lock(mMutex);
    if (!mScreen.visible()) return false;
    if (mScene && !ownsTopLocked()) return false;
    return mScreen.handleKey(keyCode);
}

bool ShaderManagerScreen::handleMouse(int action, float px, float py, bool down, int wheelDelta) {
    std::lock_guard lock(mMutex);
    if (!mScreen.visible()) return false;
    if (mScene && !ownsTopLocked()) return false;

    float const x = px;
    float const y = py;

    if (action == MouseAction::ActionMove) {
        mScreen.handleMouseMove(x, y, down);
        return true;
    }
    if (action == MouseAction::ActionWheel) return mScreen.handleWheel(wheelDelta); // 滚轮滚动设置页
    if (action != MouseAction::ActionLeft) return true;

    return mScreen.handleMouseButton(ui::MouseButton::Left, x, y, down);
}

} // namespace renderdragon_shader_loader
