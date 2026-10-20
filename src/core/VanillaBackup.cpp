#include "core/VanillaBackup.h"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace renderdragon_shader_loader {
namespace {

bool copyBinFiles(std::filesystem::path const& from, std::filesystem::path const& to, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(from, ec)) {
        if (error) *error = "source material directory does not exist: " + from.string();
        return false;
    }
    std::filesystem::create_directories(to, ec);
    std::size_t copied = 0;
    for (auto const& entry : std::filesystem::directory_iterator(from, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (!entry.path().filename().string().ends_with(".material.bin")) continue;
        std::filesystem::copy_file(
            entry.path(),
            to / entry.path().filename(),
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
        if (!ec) ++copied;
    }
    if (copied == 0) {
        if (error) *error = "no .material.bin files found in " + from.string();
        return false;
    }
    return true;
}

#ifdef _WIN32
int runHidden(std::wstring commandLine) {
    STARTUPINFOW startup{};
    startup.cb          = sizeof(startup);
    startup.dwFlags     = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process
        )) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)code;
}
#endif

} // namespace

bool ensureVanillaSnapshot(
    std::filesystem::path const& materialsDir,
    std::filesystem::path const& snapshotDir,
    std::string*                 error
) {
    std::error_code ec;
    if (std::filesystem::is_directory(snapshotDir, ec) && std::filesystem::directory_iterator(snapshotDir, ec)
        != std::filesystem::directory_iterator{}) {
        return true;
    }
    return copyBinFiles(materialsDir, snapshotDir, error);
}

bool restoreVanillaSnapshot(
    std::filesystem::path const& snapshotDir,
    std::filesystem::path const& materialsDir,
    std::string*                 error
) {
    return copyBinFiles(snapshotDir, materialsDir, error);
}

bool exportVanillaSnapshot(
    std::filesystem::path const& snapshotDir,
    std::filesystem::path const& outputZip,
    std::string*                 error
) {
    std::error_code ec;
    if (!std::filesystem::is_directory(snapshotDir, ec)) {
        if (error) *error = "snapshot directory does not exist";
        return false;
    }
    std::filesystem::create_directories(outputZip.parent_path(), ec);
    std::filesystem::remove(outputZip, ec);

#ifdef _WIN32
    auto command = L"tar.exe -a -c -f \"" + outputZip.wstring() + L"\" -C \"" + snapshotDir.wstring() + L"\" .";
    if (runHidden(command) != 0) {
        if (error) *error = "tar.exe failed to export restore package";
        return false;
    }
    return true;
#else
    (void)outputZip;
    if (error) *error = "restore package export is only implemented on Windows";
    return false;
#endif
}

} // namespace renderdragon_shader_loader
