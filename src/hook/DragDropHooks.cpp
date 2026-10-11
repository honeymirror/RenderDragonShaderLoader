#include "hook/DragDropHooks.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "ll/api/mod/NativeMod.h"
#include "ll/api/thread/ClientThreadExecutor.h"
#include "ll/api/thread/ThreadPoolExecutor.h"

#include "mod/RenderDragonShaderLoader.h"
#include "ui/ShaderManagerScreen.h"

// clang-format off
#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif
// clang-format on

namespace renderdragon_shader_loader {
namespace {

#ifdef _WIN32

std::mutex gMutex;
HWND       gGameWindow      = nullptr;
WNDPROC    gOriginalWndProc = nullptr;
bool       gInstalled       = false;

struct WindowSearch {
    DWORD pid      = 0;
    HWND  best     = nullptr;
    LONG  bestArea = 0;
    bool  titled   = false;
};

BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    RECT client{};
    LONG area = GetClientRect(hwnd, &client) ? std::max<LONG>(0, client.right - client.left)
                                                   * std::max<LONG>(0, client.bottom - client.top)
                                             : 0;

    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, (int)std::size(title));
    bool const titled = std::wstring_view(title).find(L"Minecraft") != std::wstring_view::npos;

    if (search->best == nullptr || titled || (!search->titled && area > search->bestArea)) {
        search->best     = hwnd;
        search->bestArea = area;
        search->titled   = titled;
    }
    return TRUE;
}

HWND findGameWindow() {
    WindowSearch search{GetCurrentProcessId(), nullptr, 0, false};
    EnumWindows(&enumWindowsProc, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

std::wstring lowerExtension(std::filesystem::path const& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return ext;
}

bool copyFileReplacing(
    std::filesystem::path const& source,
    std::filesystem::path const& destination,
    std::string&                 error
) {
    std::filesystem::path temp = destination;
    temp += L".incoming";

    std::error_code ec;
    std::filesystem::remove(temp, ec);
    ec.clear();
    std::filesystem::copy_file(source, temp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        DWORD const last = GetLastError();
        if (last != ERROR_SHARING_VIOLATION && last != ERROR_ACCESS_DENIED) {
            error = std::system_category().message((int)last);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::filesystem::remove(temp, ec);
    if (error.empty()) error = "file is in use";
    return false;
}

void importDroppedPaths(std::vector<std::filesystem::path> paths) {
    auto& logger = ll::mod::NativeMod::current()->getLogger();
    auto  root   = RenderDragonShaderLoader::getInstance().getPacksDir();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        logger.error("[dragdrop] cannot create shaderpacks directory: {}", ec.message());
        return;
    }

    int copied  = 0;
    int skipped = 0;
    for (auto const& source : paths) {
        std::error_code typeEc;
        if (std::filesystem::is_regular_file(source, typeEc)) {
            if (lowerExtension(source) != L".zip") {
                ++skipped;
                continue;
            }

            auto const name        = source.filename();
            auto const destination = root / name;
            if (std::filesystem::equivalent(source, destination, typeEc)) {
                ++skipped;
                continue;
            }

            std::string error;
            if (copyFileReplacing(source, destination, error)) {
                ++copied;
                logger.info("[dragdrop] imported shader pack {}", name.string());
            } else {
                ++skipped;
                logger.warn("[dragdrop] failed to import {}: {}", name.string(), error);
            }
            continue;
        }

        if (std::filesystem::is_directory(source, typeEc)) {
            auto const name        = source.filename();
            auto const destination = root / name;
            if (std::filesystem::equivalent(source, destination, typeEc)) {
                ++skipped;
                continue;
            }

            std::filesystem::copy(
                source,
                destination,
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                ec
            );
            if (ec) {
                ++skipped;
                logger.warn("[dragdrop] failed to import directory {}: {}", name.string(), ec.message());
                ec.clear();
            } else {
                ++copied;
                logger.info("[dragdrop] imported shader pack directory {}", name.string());
            }
            continue;
        }

        ++skipped;
    }

    if (copied > 0) {
        ll::thread::ClientThreadExecutor::getDefault().execute([] {
            ShaderManagerScreen::getInstance().refreshPacks();
        });
    }
    logger.info("[dragdrop] import finished: {} copied, {} skipped", copied, skipped);
}

void handleDroppedFiles(HDROP drop) {
    UINT const                         count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::filesystem::path> paths;
    paths.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        UINT const length = DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) continue;

        std::wstring buffer(length + 1, L'\0');
        if (DragQueryFileW(drop, i, buffer.data(), length + 1) == 0) continue;
        buffer.resize(length);
        paths.emplace_back(std::move(buffer));
    }
    DragFinish(drop);

    if (paths.empty()) return;
    ll::thread::ThreadPoolExecutor::getDefault().execute(
        [paths = std::move(paths)]() mutable { importDroppedPaths(std::move(paths)); }
    );
}

LRESULT CALLBACK gameWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DROPFILES) {
        handleDroppedFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    }

    WNDPROC original = nullptr;
    {
        std::lock_guard lock(gMutex);
        original = gOriginalWndProc;
    }
    if (original != nullptr) return CallWindowProcW(original, hwnd, message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

#endif

} // namespace

void installDragDropHook() {
#ifdef _WIN32
    uninstallDragDropHook();

    HWND const hwnd = findGameWindow();
    if (hwnd == nullptr) {
        ll::mod::NativeMod::current()->getLogger().warn("[dragdrop] game window not found");
        return;
    }

    auto const original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (original == nullptr) {
        ll::mod::NativeMod::current()->getLogger().warn("[dragdrop] cannot read game window procedure");
        return;
    }

    {
        std::lock_guard lock(gMutex);
        gGameWindow      = hwnd;
        gOriginalWndProc = original;
        gInstalled       = true;
    }

    DragAcceptFiles(hwnd, TRUE);
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&gameWindowProc));
    ll::mod::NativeMod::current()->getLogger().info("[dragdrop] enabled for game window");
#endif
}

void uninstallDragDropHook() {
#ifdef _WIN32
    HWND    hwnd     = nullptr;
    WNDPROC original = nullptr;
    {
        std::lock_guard lock(gMutex);
        if (!gInstalled) return;
        hwnd             = gGameWindow;
        original         = gOriginalWndProc;
        gGameWindow      = nullptr;
        gOriginalWndProc = nullptr;
        gInstalled       = false;
    }
    if (hwnd != nullptr && IsWindow(hwnd)) {
        DragAcceptFiles(hwnd, FALSE);
        if (original != nullptr) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
    }
#endif
}

} // namespace renderdragon_shader_loader
