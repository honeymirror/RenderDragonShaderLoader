// 独立 UI 预览器：不启动 Minecraft，直接在 Win32 窗口里跑模组的界面逻辑
// （src/ui/UiScreen.cpp —— 和游戏内用的是同一份代码）。
//
// 编译：xmake build ui_preview
// 运行：xmake run ui_preview [shaderpacks 目录] [GUI scale]
//   默认 shaderpacks 目录 = <源码树>/../.. shaderpacks（见下面的默认值），GUI scale = 2。
//
// 交互：鼠标移动/点击 = 悬停与点击；Esc = 关闭；↑↓←→ = 焦点；回车 = 激活；
//       [ ] = 减/加当前值；Tab = 下一个；K = 重新打开界面。
//       （数字键 1..5 曾经绑五个按钮，后来去掉了：打字会误触发打开文件夹/编译。）
//
// 技术栈：软件光栅（自己写 DIB 像素，保证 alpha 混合与游戏一致）+ GDI 只用来画文字。

#define WIN32_LEAN_AND_MEAN
// 全程用显式 W 版 API；定义 UNICODE 让 IDC_ARROW 之类的宏也是宽字符版。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/CompilerService.h"
#include "core/ShaderPack.h"
#include "ui/UiCanvas.h"
#include "ui/UiScreen.h"

using namespace renderdragon_shader_loader;
using namespace renderdragon_shader_loader::ui;

// xmake 会定义成源码树根目录（用来找 lang/*.json 与 toolchain/shaderc.exe）；
// 手动编译时不定义也能跑，只是没有翻译和内置 shaderc 路径。
#ifndef UI_PREVIEW_SOURCE_DIR
#define UI_PREVIEW_SOURCE_DIR ""
#endif

namespace {

// ---------------------------------------------------------------- 小工具

uint8_t to8(float v) { return (uint8_t)std::clamp<long>(std::lround(v * 255.f), 0, 255); }

std::wstring widen(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

// 极简 JSON 读取：只认 {"键": "值", ...} 这种平面字符串表（lang/*.json 就是这个形状）。
std::unordered_map<std::string, std::string> loadFlatJson(std::filesystem::path const& file) {
    std::unordered_map<std::string, std::string> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();

    size_t pos = 0;
    auto   readString = [&](size_t& p, std::string& value) -> bool {
        while (p < text.size() && text[p] != '"') ++p;
        if (p >= text.size()) return false;
        ++p; // 开引号
        value.clear();
        while (p < text.size() && text[p] != '"') {
            if (text[p] == '\\' && p + 1 < text.size()) {
                ++p;
                char c = text[p];
                value.push_back(c == 'n' ? '\n' : c);
            } else {
                value.push_back(text[p]);
            }
            ++p;
        }
        if (p < text.size()) ++p; // 闭引号
        return true;
    };

    while (pos < text.size()) {
        std::string key;
        if (!readString(pos, key)) break;
        while (pos < text.size() && text[pos] != ':') ++pos;
        if (pos >= text.size()) break;
        ++pos;
        std::string value;
        if (!readString(pos, value)) break;
        out[key] = value;
    }
    return out;
}

// ---------------------------------------------------------------- 画布

struct Surface {
    HDC      dc     = nullptr;
    HBITMAP  bitmap = nullptr;
    uint32_t* bits  = nullptr;
    int      width  = 0;
    int      height = 0;
};

// 软件光栅 + GDI 文字：矩形自己按 alpha 混进 DIB，文字用 GDI 画到同一个 DIB。
class GdiCanvas final : public Canvas {
public:
    GdiCanvas(Surface& surface, float scale) : mSurface(surface), mScale(scale) {}

    ~GdiCanvas() override {
        for (auto& [key, font] : mFonts) DeleteObject(font);
    }

    void fill(float x, float y, float w, float h, Color c, float alpha) override {
        if (w <= 0.f || h <= 0.f) return;

        int x0 = (int)std::lround(x * mScale);
        int y0 = (int)std::lround(y * mScale);
        int x1 = (int)std::lround((x + w) * mScale);
        int y1 = (int)std::lround((y + h) * mScale);
        x0      = std::max(x0, 0);
        y0      = std::max(y0, 0);
        x1      = std::min(x1, mSurface.width);
        y1      = std::min(y1, mSurface.height);
        if (x1 <= x0 || y1 <= y0) return;

        float const a = std::clamp(c.a * alpha, 0.f, 1.f);
        if (a <= 0.001f) return;

        uint8_t const sr = to8(c.r), sg = to8(c.g), sb = to8(c.b);

        if (a >= 0.999f) {
            uint32_t const px = 0xFF000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | sb;
            for (int yy = y0; yy < y1; ++yy) {
                uint32_t* row = mSurface.bits + (size_t)yy * (size_t)mSurface.width;
                std::fill(row + x0, row + x1, px);
            }
            return;
        }

        int const ia = (int)std::lround(a * 255.f);
        int const ka = 255 - ia;
        for (int yy = y0; yy < y1; ++yy) {
            uint32_t* row = mSurface.bits + (size_t)yy * (size_t)mSurface.width;
            for (int xx = x0; xx < x1; ++xx) {
                uint32_t const d = row[xx];
                int const dr = (int)((d >> 16) & 0xFF);
                int const dg = (int)((d >> 8) & 0xFF);
                int const db = (int)(d & 0xFF);
                int const r  = (sr * ia + dr * ka + 127) / 255;
                int const g  = (sg * ia + dg * ka + 127) / 255;
                int const b  = (sb * ia + db * ka + 127) / 255;
                row[xx] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    void text(float x, float y, float boxWidth, std::string const& value, Color c, Align align, float size) override {
        if (value.empty() || boxWidth <= 0.f) return;

        RECT rect{};
        rect.left   = (int)std::lround(x * mScale);
        rect.top    = (int)std::lround(y * mScale);
        rect.right  = (int)std::lround((x + boxWidth) * mScale);
        rect.bottom = (int)std::lround((y + 14.f * size) * mScale);

        HFONT const font = fontFor(size);
        HGDIOBJ     old  = SelectObject(mSurface.dc, font);
        SetBkMode(mSurface.dc, TRANSPARENT);
        // GDI 的 DrawText 不吃 alpha：用"朝深灰压"近似出淡出（只有状态提示会用到 a<1）。
        float const a = std::clamp(c.a, 0.f, 1.f);
        if (a <= 0.02f) {
            SelectObject(mSurface.dc, old);
            return;
        }
        auto mix = [a](float v) { return to8(std::clamp(v * a + 0.09f * (1.f - a), 0.f, 1.f)); };
        SetTextColor(mSurface.dc, RGB(mix(c.r), mix(c.g), mix(c.b)));

        UINT format = DT_SINGLELINE | DT_TOP | DT_NOPREFIX | DT_END_ELLIPSIS;
        if (align == Align::Center) format |= DT_CENTER;
        else if (align == Align::Right) format |= DT_RIGHT;

        std::wstring const wide = widen(value);
        DrawTextW(mSurface.dc, wide.c_str(), (int)wide.size(), &rect, format);
        SelectObject(mSurface.dc, old);
    }

    // 量文字宽度（GUI 单位）：给"跟着鼠标走的滑块名字浮层"定宽用。
    [[nodiscard]] float textWidth(std::string const& value, float size) const override {
        if (value.empty()) return 0.f;
        HFONT const font = const_cast<GdiCanvas*>(this)->fontFor(size);
        HGDIOBJ     old  = SelectObject(mSurface.dc, font);
        SIZE        extent{};
        std::wstring const wide = widen(value);
        GetTextExtentPoint32W(mSurface.dc, wide.c_str(), (int)wide.size(), &extent);
        SelectObject(mSurface.dc, old);
        return (float)extent.cx / std::max(mScale, 0.01f);
    }

private:
    HFONT fontFor(float size) {
        int const height = -(int)std::lround(9.5f * size * mScale);
        auto      it     = mFonts.find(height);
        if (it != mFonts.end()) return it->second;

        HFONT font = CreateFontW(
            height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI"
        );
        mFonts.emplace(height, font);
        return font;
    }

    Surface&                      mSurface;
    float                         mScale;
    std::map<int, HFONT>          mFonts;
};

// ---------------------------------------------------------------- 应用状态

struct App {
    Screen screen;
    float  scale     = 2.f;
    int    pixelW    = 0;
    int    pixelH    = 0;
    Surface surface;
    bool   compileEnabled = true;
};

App g_app;

HINSTANCE g_instance = nullptr;

void logLine(std::string const& line); // 定义在下面

// 取窗口像素尺寸对应的 GUI 单位尺寸（和游戏里 pixel / guiScale 一致）。
float guiWidth() { return (float)g_app.pixelW / g_app.scale; }
float guiHeight() { return (float)g_app.pixelH / g_app.scale; }

void ensureSurface(int width, int height) {
    if (g_app.surface.bitmap != nullptr && g_app.surface.width == width && g_app.surface.height == height) return;
    if (g_app.surface.dc != nullptr) {
        DeleteObject(g_app.surface.bitmap);
        DeleteDC(g_app.surface.dc);
        g_app.surface = {};
    }
    if (width <= 0 || height <= 0) return;

    HDC const screen = GetDC(nullptr);
    g_app.surface.dc = CreateCompatibleDC(screen);

    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth       = width;
    info.bmiHeader.biHeight      = -height; // top-down
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g_app.surface.bitmap = CreateDIBSection(g_app.surface.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_app.surface.bitmap == nullptr) {
        DeleteDC(g_app.surface.dc);
        g_app.surface = {};
        ReleaseDC(nullptr, screen);
        return;
    }
    SelectObject(g_app.surface.dc, g_app.surface.bitmap);
    g_app.surface.bits   = static_cast<uint32_t*>(bits);
    g_app.surface.width  = width;
    g_app.surface.height = height;
    ReleaseDC(nullptr, screen);
}

// 假世界背景：一个暗色渐变，近似游戏里那片天空/地形，用来判断面板对比度。
void paintFakeWorld(int width, int height) {
    for (int y = 0; y < height; ++y) {
        float     t  = (float)y / (float)std::max(height - 1, 1);
        int       r  = (int)(28 + 60 * (1.f - t));
        int       g  = (int)(34 + 80 * (1.f - t));
        int       b  = (int)(46 + 96 * (1.f - t));
        uint32_t  px = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        uint32_t* row = g_app.surface.bits + (size_t)y * (size_t)width;
        std::fill(row, row + width, px);
    }
}

// 32bpp BMP 输出（bottom-up），给无窗口截图模式用。
bool writeBmp32(std::filesystem::path const& file, Surface const& s) {
    if (s.bits == nullptr || s.width <= 0 || s.height <= 0) return false;
    std::ofstream out(file, std::ios::binary);
    if (!out) return false;

    uint32_t const rowBytes  = (uint32_t)s.width * 4u;
    uint32_t const imageSize = rowBytes * (uint32_t)s.height;

    uint8_t header[54]{};
    header[0] = 'B';
    header[1] = 'M';
    auto put32 = [&](int off, uint32_t v) {
        header[off + 0] = (uint8_t)(v & 0xFFu);
        header[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
        header[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
        header[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
    };
    put32(2, 54u + imageSize);
    put32(10, 54u);
    put32(14, 40u);
    put32(18, (uint32_t)s.width);
    put32(22, (uint32_t)s.height);
    header[26] = 1;
    header[28] = 32;
    put32(34, imageSize);
    out.write((char const*)header, 54);
    for (int y = s.height - 1; y >= 0; --y) {
        out.write((char const*)(s.bits + (size_t)y * (size_t)s.width), (std::streamsize)rowBytes);
    }
    return (bool)out;
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC const   dc = BeginPaint(hwnd, &ps);
    if (dc == nullptr) return;

    RECT client{};
    GetClientRect(hwnd, &client);
    int const width  = std::max((int)(client.right - client.left), 1);
    int const height = std::max((int)(client.bottom - client.top), 1);

    ensureSurface(width, height);
    if (g_app.surface.bitmap == nullptr) {
        EndPaint(hwnd, &ps);
        return;
    }
    g_app.pixelW = width;
    g_app.pixelH = height;

    paintFakeWorld(width, height);

    GdiCanvas canvas(g_app.surface, g_app.scale);
    g_app.screen.render(canvas, guiWidth(), guiHeight());

    BitBlt(dc, 0, 0, width, height, g_app.surface.dc, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);

    // 心跳：崩溃时用日志的最后一行定位。
    static int frame = 0;
    if (++frame % 20 == 0) logLine("[preview] frame " + std::to_string(frame) + " client " + std::to_string(width) +
                                   "x" + std::to_string(height));
}

std::ofstream g_log; // 文件日志：程序崩了也能看到最后几行（stdout 在 AllocConsole 后未必可达）

void logLine(std::string const& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (g_log) {
        g_log << line << "\n";
        g_log.flush();
    }
}

// 顶栏「导入 / 导出」用的系统文件对话框（和游戏内那套同一做法，方便离线试）。
std::filesystem::path pickTextFile(std::filesystem::path const& initialDir, std::string const& suggestedName,
                                   bool forWrite) {
    wchar_t buffer[1024]{};
    if (!suggestedName.empty()) {
        std::wstring const wide = widen(suggestedName);
        wcsncpy_s(buffer, std::size(buffer), wide.c_str(), _TRUNCATE);
    }
    std::wstring const dir = initialDir.wstring();

    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = GetActiveWindow();
    ofn.lpstrFilter     = L"配置文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile       = buffer;
    ofn.nMaxFile        = (DWORD)std::size(buffer);
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt     = L"txt";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY |
                (forWrite ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));

    BOOL const ok = forWrite ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return {};
    return std::filesystem::path(buffer);
}

std::string readWholeFile(std::filesystem::path const& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool writeWholeFile(std::filesystem::path const& file, std::string const& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), (std::streamsize)text.size());
    return out.good();
}

void openFolder(std::filesystem::path const& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::wstring command = L"explorer.exe \"" + std::filesystem::absolute(dir, ec).wstring() + L"\"";
    STARTUPINFOW        si{};
    si.cb          = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// 预览器里的「编译」真的会调 lazurite（和游戏内同一条 CompilerService），
// 想只看界面可以加 --no-compile。
bool compilePack(ShaderPack const& pack) {
    if (!g_app.compileEnabled) {
        logLine("[preview] compile disabled (--no-compile); pack = " + pack.name);
        return false;
    }
    std::vector<std::string> macros;
    for (auto const& parameter : pack.parameters) {
        auto macro = toCompileMacro(parameter);
        if (!macro.empty()) macros.push_back(std::move(macro));
    }
    std::vector<UniformPatch> uniformPatches;
    std::string uniformError;
    if (!collectUniformPatches(pack.parameters, uniformPatches, uniformError)) {
        logLine("[preview] " + uniformError);
        return false;
    }
    logLine(
        "[preview] compiling '" + pack.name + "' with " + std::to_string(macros.size())
        + " macro(s), " + std::to_string(uniformPatches.size()) + " uniform patch(es)"
    );
    CompileOptions options;
    options.macros         = std::move(macros);
    options.uniformPatches = std::move(uniformPatches);
    options.profiles       = pack.profiles;
    options.outputDir      = pack.binsDir;
    options.cacheDir       = pack.cacheDir;
    options.packName       = pack.name;
    options.preset         = pack.currentPreset;
    options.platform       = "Direct3D_SM65";
    bool started = CompilerService::getInstance().compile(pack.sourceDir, std::move(options));
    if (started) ShaderPackStore::saveSettings(pack);
    return started;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE: {
        g_app.screen.setPointer((float)GET_X_LPARAM(lp) / g_app.scale, (float)GET_Y_LPARAM(lp) / g_app.scale);
        bool down = (wp & MK_LBUTTON) != 0;
        g_app.screen.handleMouseMove((float)GET_X_LPARAM(lp) / g_app.scale,
                                     (float)GET_Y_LPARAM(lp) / g_app.scale, down);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        g_app.screen.setPointer((float)GET_X_LPARAM(lp) / g_app.scale, (float)GET_Y_LPARAM(lp) / g_app.scale);
        g_app.screen.handleMouseButton(MouseButton::Left, (float)GET_X_LPARAM(lp) / g_app.scale,
                                       (float)GET_Y_LPARAM(lp) / g_app.scale, true);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        ReleaseCapture();
        g_app.screen.handleMouseButton(MouseButton::Left, (float)GET_X_LPARAM(lp) / g_app.scale,
                                       (float)GET_Y_LPARAM(lp) / g_app.scale, false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        int const key = (int)wp;
        if (key == 'K') {
            if (!g_app.screen.visible()) g_app.screen.open();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        g_app.screen.handleKey(key);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        // 编译过程中界面要跟着刷新（进度文字）。
        if (g_app.screen.visible()) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1; // 全部由 paint 负责

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

std::filesystem::path defaultPacksDir() {
    // Never embed a developer-specific installation path in source or binaries.
    std::error_code ec;
    if (char const* explicitDir = std::getenv("RDL_PREVIEW_PACKS_DIR");
        explicitDir != nullptr && *explicitDir != '\0'
        && std::filesystem::is_directory(explicitDir, ec)) {
        return explicitDir;
    }
    if (char const* gameDir = std::getenv("RDL_GAME_DIR");
        gameDir != nullptr && *gameDir != '\0') {
        auto candidates = std::filesystem::path(gameDir) / "data" / "shaderpacks";
        if (std::filesystem::is_directory(candidates, ec)) return candidates;
    }
    return std::filesystem::current_path() / "data" / "shaderpacks";
}

} // namespace

int main(int argc, char** argv) {
    // 日志文件（放 exe 旁边）：崩溃时也能看到最后一条。
    {
        std::error_code ec;
        auto            exeDir = std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
        g_log.open(exeDir / "ui_preview.log", std::ios::trunc);
    }
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
        char buf[256];
        std::snprintf(
            buf,
            sizeof(buf),
            "[preview] CRASH code=0x%08lX address=%p",
            (unsigned long)info->ExceptionRecord->ExceptionCode,
            info->ExceptionRecord->ExceptionAddress
        );
        logLine(buf);
        return EXCEPTION_EXECUTE_HANDLER;
    });

    // 控制台：方便看日志（xmake run 时直接复用到父控制台）。
    if (GetConsoleWindow() == nullptr) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
    }
    SetConsoleOutputCP(CP_UTF8);

    std::filesystem::path packsDir = defaultPacksDir();
    bool                  packsDirSet = false;
    std::filesystem::path shotPath;   // --shot <file.bmp>：无窗口渲染一帧后写 BMP 退出
    std::string           shotPage = "list";
    int                   shotGuiW = 640, shotGuiH = 371;
    float                 shotPointerX = -1.f, shotPointerY = -1.f; // --pointer x,y
    std::vector<std::pair<float, float>> shotClicks;                // --click x,y（可多次，按顺序点）
    std::vector<std::array<float, 4>>    shotDrags;                 // --drag x1,y1,x2,y2（按住左键拖动）
    std::vector<int>                     shotKeys;                 // --key <vk> / --keys 40,39,13（键盘序列）
    int                                  shotWheel = 0;             // --wheel N：向下滚 N 格（负数向上）
    int                                  shotWaitMs = 0;            // --wait N：最后再等 N 毫秒（测淡出这类按时间变化的效果）
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--no-compile") {
            g_app.compileEnabled = false;
        } else if (arg == "--scale" && i + 1 < argc) {
            g_app.scale = (float)std::atof(argv[++i]);
        } else if (arg == "--shot" && i + 1 < argc) {
            shotPath = argv[++i];
        } else if (arg == "--page" && i + 1 < argc) {
            shotPage = argv[++i];
        } else if (arg == "--size" && i + 1 < argc) {
            std::string size = argv[++i];
            auto        x    = size.find('x');
            if (x != std::string::npos) {
                shotGuiW = std::max(std::atoi(size.substr(0, x).c_str()), 64);
                shotGuiH = std::max(std::atoi(size.substr(x + 1).c_str()), 64);
            }
        } else if (arg == "--pointer" && i + 1 < argc) {
            std::string p = argv[++i];
            auto        c = p.find(',');
            if (c != std::string::npos) {
                shotPointerX = (float)std::atof(p.substr(0, c).c_str());
                shotPointerY = (float)std::atof(p.substr(c + 1).c_str());
            }
        } else if (arg == "--click" && i + 1 < argc) {
            std::string p = argv[++i];
            auto        c = p.find(',');
            if (c != std::string::npos) {
                shotClicks.emplace_back(
                    (float)std::atof(p.substr(0, c).c_str()),
                    (float)std::atof(p.substr(c + 1).c_str())
                );
            }
        } else if (arg == "--drag" && i + 1 < argc) {
            // --drag x1,y1,x2,y2：按住左键从 (x1,y1) 拖到 (x2,y2)，验证滑块拖动。
            std::string p = argv[++i];
            float       v[4] = {0, 0, 0, 0};
            int         k    = 0;
            std::size_t pos  = 0;
            while (k < 4 && pos <= p.size()) {
                auto next = p.find(',', pos);
                if (next == std::string::npos) next = p.size();
                v[k++]    = (float)std::atof(p.substr(pos, next - pos).c_str());
                pos       = next + 1;
            }
            if (k == 4) shotDrags.push_back({v[0], v[1], v[2], v[3]});
        } else if (arg == "--wheel" && i + 1 < argc) {
            shotWheel = std::atoi(argv[++i]);
        } else if (arg == "--key" && i + 1 < argc) {
            shotKeys.push_back((int)std::strtol(argv[++i], nullptr, 0));
        } else if (arg == "--keys" && i + 1 < argc) {
            // "40,39,13" 这样的逗号分隔虚拟键码序列（40=下 39=右 38=上 37=左 13=回车 27=Esc）
            std::string seq = argv[++i];
            std::size_t pos = 0;
            while (pos <= seq.size()) {
                std::size_t end = seq.find(',', pos);
                if (end == std::string::npos) end = seq.size();
                std::string part = seq.substr(pos, end - pos);
                if (!part.empty()) shotKeys.push_back((int)std::strtol(part.c_str(), nullptr, 0));
                if (end == seq.size()) break;
                pos = end + 1;
            }
        } else if (arg == "--wait" && i + 1 < argc) {
            shotWaitMs = std::atoi(argv[++i]);
        } else if (!packsDirSet && !arg.empty() && arg[0] != '-') {
            packsDir    = arg;
            packsDirSet = true;
        }
    }
    if (g_app.scale < 0.5f) g_app.scale = 0.5f;

    std::filesystem::path const generatedDir = packsDir.parent_path() / "generated";
    std::filesystem::path const presetsDir   = packsDir.parent_path() / "presets";

    // ---- 扫描真实着色器包（和游戏内同一个 ShaderPackStore）----
    ShaderPackStore::getInstance().scan(packsDir, generatedDir, presetsDir);
    auto const& packs = ShaderPackStore::getInstance().packs();
    logLine("[preview] shaderpacks: " + packsDir.string());
    logLine("[preview] generated  : " + generatedDir.string());
    logLine("[preview] presets    : " + presetsDir.string());
    for (auto const& pack : packs) {
        logLine("[preview] pack '" + pack.name + "'  " + std::to_string(pack.parameters.size()) + " parameter(s)");
    }
    if (packs.empty()) logLine("[preview] (no packs found)");

    // ---- 源码树定位（找 lang/*.json 与 toolchain/shaderc.exe）----
    std::filesystem::path sourceDir;
    if (char const* fromBuild = UI_PREVIEW_SOURCE_DIR; fromBuild != nullptr && *fromBuild != '\0') {
        std::error_code ec;
        if (std::filesystem::is_directory(fromBuild, ec)) sourceDir = fromBuild;
    }
    if (sourceDir.empty()) {
        // 兜底：从 exe 所在目录往上找带 lang/ 与 toolchain/ 的那一级。
        std::error_code ec;
        auto            dir = std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
        for (int i = 0; i < 6 && !dir.empty(); ++i) {
            if (std::filesystem::is_directory(dir / "lang", ec) &&
                std::filesystem::is_directory(dir / "toolchain", ec)) {
                sourceDir = dir;
                break;
            }
            dir = dir.parent_path();
        }
    }

    // ---- 文案：直接读模组的 lang/zh_CN.json ----
    std::unordered_map<std::string, std::string> strings;
    if (!sourceDir.empty()) {
        logLine("[preview] source tree: " + sourceDir.string());
        strings = loadFlatJson(sourceDir / "lang" / "zh_CN.json");
        if (strings.empty()) {
            strings = loadFlatJson(sourceDir / "lang" / "en_US.json");
        }

        // 编译后端：用仓库里自带的 toolchain/shaderc.exe。
        CompilerSettings settings;
        settings.workingDir      = sourceDir;
        settings.outputDir       = generatedDir;
        settings.lazuriteCommand = "py -3 toolchain/lazurite_albite.py";
        settings.shadercPath     = (sourceDir / "toolchain" / "shaderc.exe").string();
        if (!std::filesystem::exists(settings.shadercPath)) settings.shadercPath.clear();
        settings.skipValidation  = true;
        CompilerService::getInstance().configure(settings);
    }

    // ---- 注入宿主能力：预览器版 ----
    ScreenHost host;
    host.translate = [&strings](std::string_view key) -> std::string {
        auto it = strings.find(std::string(key));
        return it == strings.end() ? std::string{} : it->second;
    };
    host.log = [](std::string const& line) { logLine(line); };
    // 预览器没有游戏语言：留空，让包内语言文件按内置回退顺序挑。
    host.locale = []() { return std::string{}; };
    host.compile = [](ShaderPack const& pack) {
        return compilePack(pack) ? ui::CompileRequestResult::Queued
                                 : ui::CompileRequestResult::Failed;
    };
    host.preflight = [](ShaderPack const& pack) {
        ui::CompatibilityReport report;
        report.renderMode = pack.renderMode == ShaderRenderMode::Deferred ? "Deferred" : "Forward";
        std::error_code ec;
        report.overridesFound = std::filesystem::is_regular_file(
            pack.sourceDir / "rdloader.io-overrides.json",
            ec
        );
        if (std::filesystem::is_directory(pack.binsDir, ec)) {
            for (std::filesystem::directory_iterator it(pack.binsDir, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec)
                    && it->path().filename().string().ends_with(".material.bin")) {
                    ++report.materialCount;
                }
            }
        }
        report.compiled = report.materialCount > 0;
        if (!report.compiled) {
            report.detail = "This pack has no compiled material output yet.";
            return report;
        }
        std::string    error;
        NativeIOReport ioReport;
        report.ok =
            CompilerService::getInstance().validateNativeIO(pack.sourceDir, pack.binsDir, ioReport, error);
        report.nativeIO = std::move(ioReport);
        report.detail = report.ok ? "Material IO contract matched the target engine." : error;
        logLine(
            "[preview] preflight materials=" + std::to_string(report.materialCount)
            + " ok=" + (report.ok ? "true" : "false")
            + " errors=" + std::to_string(report.nativeIO.errors)
            + " warnings=" + std::to_string(report.nativeIO.warnings)
        );
        return report;
    };
    host.exportCompatibilityReport =
        [&generatedDir](ShaderPack const& pack, std::string const& text, std::string& outputPath) -> bool {
        auto const dir = generatedDir.parent_path() / "reports" / "compatibility";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;
        auto const file = dir / (pack.name + ".json");
        if (!writeWholeFile(file, text)) return false;
        outputPath = std::filesystem::absolute(file, ec).string();
        if (ec) outputPath = file.string();
        return true;
    };
    host.reload = [](ShaderPack const& pack) -> bool {
        logLine("[preview] reload requested -> " + pack.binsDir.string() + "（游戏内才会真的重载材质）");
        return true;
    };
    host.restoreVanilla = []() { logLine("[preview] shaders disabled -> 原版材质（游戏内会真的还原）"); };
    host.rescanPacks = [&packsDir, &generatedDir]() {
        ShaderPackStore::getInstance().scan(packsDir, generatedDir); // 预览器里也能热刷新列表
    };
    host.openFolder = [&packsDir]() { openFolder(packsDir); };
    host.openConfigurationFolder = [&presetsDir](ShaderPack const& pack) {
        auto dir = presetsDir / pack.name;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        openFolder(dir);
    };
    // 设置页顶栏的导入/导出：预览器也弹系统文件对话框（游戏内同一套做法）。
    host.importSettings = [&packsDir](std::string const& suggestedName, std::string& outText) -> bool {
        auto const picked = pickTextFile(packsDir, suggestedName, false);
        if (picked.empty()) {
            logLine("[preview] import cancelled");
            return false;
        }
        outText = readWholeFile(picked);
        if (outText.empty()) {
            logLine("[preview] import read failed -> " + picked.string());
            return false;
        }
        std::error_code ec;
        std::filesystem::copy_file(picked, packsDir / suggestedName,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        logLine("[preview] import " + picked.string() + " -> " + (packsDir / suggestedName).string());
        return true;
    };
    host.exportSettings = [&packsDir](std::string const& suggestedName, std::string const& text) -> bool {
        auto const picked = pickTextFile(packsDir, suggestedName, true);
        if (picked.empty()) {
            logLine("[preview] export cancelled");
            return false;
        }
        if (!writeWholeFile(picked, text)) {
            logLine("[preview] export write failed -> " + picked.string());
            return false;
        }
        writeWholeFile(packsDir / suggestedName, text);
        logLine("[preview] export -> " + picked.string());
        return true;
    };
    // 游戏里这里播原版 UI 点击音（random.click）；预览器只留一行日志。
    host.playClick = []() { logLine("[preview] click sound (game: random.click)"); };
    host.closeScreen = []() {
        logLine("[preview] screen closed");
        PostQuitMessage(0);
    };
    host.compiling      = []() { return CompilerService::getInstance().running(); };
    host.statusMessage  = []() { return CompilerService::getInstance().lastMessage(); };
    host.statusFailed   = []() { return CompilerService::getInstance().status() == CompileStatus::Failed; };

    // 模组设置页（左下角「设置」）：预览器里用一份内存配置，随便改都不碰游戏目录。
    static std::string previewUiKey          = "G";
    static bool        previewVibrant        = true;
    static bool        previewAutoLoadLast   = true;
    static int         previewModeOverride   = 0;
    host.readSetting = [](std::string const& key) -> std::string {
        if (key == "uiKey") return previewUiKey;
        if (key == "forceVibrantVisuals") return previewVibrant ? "ON" : "OFF";
        if (key == "autoLoadLastShader") return previewAutoLoadLast ? "ON" : "OFF";
        if (key == "graphicsModeOverride") return std::to_string(previewModeOverride);
        return {};
    };
    host.writeSetting = [](std::string const& key, std::string const& value) -> bool {
        if (key == "uiKey") {
            previewUiKey = value;
        } else if (key == "forceVibrantVisuals") {
            previewVibrant = (value == "ON");
        } else if (key == "autoLoadLastShader") {
            previewAutoLoadLast = (value == "ON");
        } else if (key == "graphicsModeOverride") {
            previewModeOverride = std::atoi(value.c_str());
        }
        logLine("[preview] setting " + key + " = " + value);
        return true;
    };
    g_app.screen.setHost(std::move(host));
    g_app.screen.open();

    // ---- 无窗口截图：渲染一帧写 BMP 后退出（调界面用，不用弹窗、更不用开游戏）----
    if (!shotPath.empty()) {
        int const pixelW = std::max((int)std::lround((float)shotGuiW * g_app.scale), 64);
        int const pixelH = std::max((int)std::lround((float)shotGuiH * g_app.scale), 64);
        ensureSurface(pixelW, pixelH);
        if (g_app.surface.bitmap == nullptr) {
            logLine("[preview] --shot: cannot create surface");
            CompilerService::getInstance().shutdown();
            return 1;
        }
        g_app.pixelW = pixelW;
        g_app.pixelH = pixelH;

        // 渲染一帧（界面里的命中测试用上一帧的版式）。游戏里每帧都在渲染，
        // 脚本化点击也照做：每次点击后重画一帧，模拟"渲染 → 输入 → 渲染"的循环。
        auto repaint = [&]() {
            paintFakeWorld(pixelW, pixelH);
            GdiCanvas canvas(g_app.surface, g_app.scale);
            g_app.screen.render(canvas, guiWidth(), guiHeight());
        };
        repaint();

        // 脚本化点击辅助：界面里对"同一坐标的连点"有 200ms 去抖，所以每次点击之间要等一会。
        auto click = [](float cx, float cy) {
            g_app.screen.setPointer(cx, cy);
            g_app.screen.handleMouseButton(MouseButton::Left, cx, cy, true);
            g_app.screen.handleMouseButton(MouseButton::Left, cx, cy, false);
        };
        auto clickButton = [&](int index) {
            Layout const L = buildLayout(guiWidth(), guiHeight());
            if (index < 0 || index >= L.buttonCount) return;
            click((L.buttons[index].x0 + L.buttons[index].x1) / 2.f, (L.buttons[index].y0 + L.buttons[index].y1) / 2.f);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            repaint();
        };

        // settings：先点一次「光影包设置...」（按钮 1）进入 Iris 风格的参数设置页。
        if (shotPage == "settings" || shotPage == "groups" || shotPage == "options") clickButton(1);
        // prefs：点左下角那个「设置」按钮，进模组设置页（面板热键 / 强制灵动视效 / 自动加载）。
        if (shotPage == "prefs") {
            Layout const L = buildLayout(guiWidth(), guiHeight());
            if (L.settingsButton.valid()) {
                click((L.settingsButton.x0 + L.settingsButton.x1) / 2.f,
                      (L.settingsButton.y0 + L.settingsButton.y1) / 2.f);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                repaint();
            }
        }

        // --key / --keys：喂一串虚拟键码（验证方向键焦点、录键位这些键盘路径）。
        for (int vk : shotKeys) {
            g_app.screen.handleKey(vk);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            repaint();
        }

        // --click 指定的点击（按顺序、带间隔）
        for (auto const& [cx, cy] : shotClicks) {
            click(cx, cy);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            repaint();
        }

        // --drag 指定的拖动：按下左键后靠"移动指针"推进（游戏里移动走 setPointer），
        // 最后再松开；用来验证"按住左键拖滑块"这条路径。
        for (auto const& d : shotDrags) {
            g_app.screen.setPointer(d[0], d[1]);
            // 先把指针位置渲染一帧：游戏里每帧都在重画，悬停会把滑块展开成整格轨道，
            // 之后的按下才落在"轨道"上（否则会按旧版式命中右边那个小值框）。
            repaint();
            g_app.screen.handleMouseButton(MouseButton::Left, d[0], d[1], true);
            constexpr int steps = 8;
            for (int i = 1; i <= steps; ++i) {
                float const t = (float)i / (float)steps;
                float const x = d[0] + (d[2] - d[0]) * t;
                float const y = d[1] + (d[3] - d[1]) * t;
                g_app.screen.setPointer(x, y);          // 游戏内的路径
                g_app.screen.handleMouseMove(x, y, true); // 预览窗口内的路径
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            g_app.screen.handleMouseButton(MouseButton::Left, d[2], d[3], false);
            logLine("[preview] --drag done");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            repaint();
        }

        // --wheel N：向下滚 N 格（负数向上），模拟游戏里的滚轮（handleWheel 的 delta 用 ±1）。
        for (int k = 0; k < std::abs(shotWheel); ++k) {
            g_app.screen.handleWheel(shotWheel > 0 ? -1 : 1);
        }
        if (shotWheel != 0) repaint();

        // --wait N：再等一会儿再出图（淡出这类"按时间变化"的效果靠它验证）。
        if (shotWaitMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(shotWaitMs));
            repaint();
        }

        // 悬停/键盘焦点演示：把指针放到第 2 个按钮中间，截图里能看到"选中变白"。
        if (shotPage == "hover" && shotPointerX < 0.f) {
            Layout const L = buildLayout(guiWidth(), guiHeight());
            if (L.buttonCount > 1) {
                float const cx = (L.buttons[1].x0 + L.buttons[1].x1) / 2.f;
                float const cy = (L.buttons[1].y0 + L.buttons[1].y1) / 2.f;
                shotPointerX   = cx;
                shotPointerY   = cy;
            }
        }
        if (shotPointerX >= 0.f && shotPointerY >= 0.f) {
            g_app.screen.setPointer(shotPointerX, shotPointerY);
        }

        repaint();

        bool const ok = writeBmp32(shotPath, g_app.surface);
        logLine(std::string("[preview] --shot ") + (ok ? "written -> " : "FAILED -> ") + shotPath.string());
        CompilerService::getInstance().shutdown();
        return ok ? 0 : 1;
    }

    // ---- 窗口 ----
    SetProcessDPIAware();
    g_instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g_instance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"RDSLUiPreview";
    RegisterClassExW(&wc);

    int const guiW = 640, guiH = 371;
    RECT      rect{0, 0, (LONG)std::lround(guiW * g_app.scale), (LONG)std::lround(guiH * g_app.scale)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND const hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"RenderDragonShaderLoader — UI preview (脱离游戏的界面预览)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
        nullptr, g_instance, nullptr
    );
    if (hwnd == nullptr) {
        logLine("[preview] CreateWindow failed");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 100, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CompilerService::getInstance().shutdown();
    return 0;
}
