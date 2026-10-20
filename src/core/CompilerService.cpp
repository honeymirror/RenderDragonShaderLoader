#include "core/CompilerService.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <nlohmann/json.hpp>

#include "core/CompileCache.h"
#include "core/CompileStages.h"
#include "core/Hashing.h"
#include "core/MacroDependency.h"
#include "core/MaterialValidation.h"
#include "core/SourcePatcher.h"

namespace renderdragon_shader_loader {
namespace {

constexpr std::size_t maxLogLines = 4000;
constexpr int         pollMillis   = 100;

std::string quoteArg(std::string const& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(*it);
        }
    }
    out.push_back('"');
    return out;
}

std::vector<std::string> splitCommandPrefix(std::string const& command) {
    std::vector<std::string> tokens;
    std::string              current;
    bool                     inQuotes = false;
    for (char c : command) {
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (!inQuotes && (c == ' ' || c == '\t')) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

bool isExecutableFile(std::filesystem::path const& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    auto size = std::filesystem::file_size(path, ec);
    return !ec && size > 0;
}

std::filesystem::path absolutePath(std::filesystem::path const& path) {
    if (path.empty()) return {};
    std::error_code ec;
    auto            result = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    if (ec) return path;
    return result;
}

std::filesystem::path firstExisting(std::vector<std::filesystem::path> const& candidates) {
    for (auto const& candidate : candidates) {
        if (isExecutableFile(candidate)) return candidate;
    }
    return {};
}

std::filesystem::path resolveProgram(std::string const& program) {
    if (program.empty()) return {};

    std::filesystem::path asPath(program);
    if (asPath.has_parent_path() && isExecutableFile(asPath)) return asPath;

#ifdef _WIN32
    auto normalized = program;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    auto localAppData = [] {
        std::wstring value(32768, L'\0');
        DWORD        length = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), (DWORD)value.size());
        if (length == 0 || length >= value.size()) return std::filesystem::path{};
        value.resize(length);
        return std::filesystem::path(value);
    }();

    auto searchPath = [&](std::wstring const& name) -> std::filesystem::path {
        std::wstring buffer(32768, L'\0');
        DWORD        length = SearchPathW(nullptr, name.c_str(), nullptr, (DWORD)buffer.size(), buffer.data(), nullptr);
        if (length == 0 || length >= buffer.size()) return {};
        buffer.resize(length);
        return std::filesystem::path(buffer);
    };

    if (normalized == "py") {
        std::vector<std::filesystem::path> candidates;
        if (!localAppData.empty()) {
            candidates.push_back(localAppData / "Programs" / "Python" / "Launcher" / "py.exe");
            candidates.push_back(localAppData / "Microsoft" / "WindowsApps" / "py.exe");
        }
        candidates.push_back(searchPath(L"py.exe"));
        return firstExisting(candidates);
    }

    if (normalized == "python" || normalized == "python3") {
        std::vector<std::filesystem::path> candidates;
        if (!localAppData.empty()) {
            std::error_code ec;
            auto             pythonRoot = localAppData / "Python";
            if (std::filesystem::is_directory(pythonRoot, ec)) {
                for (auto const& entry : std::filesystem::directory_iterator(pythonRoot, ec)) {
                    if (!entry.is_directory(ec)) continue;
                    candidates.push_back(entry.path() / "python.exe");
                }
            }
            candidates.push_back(localAppData / "Python" / "bin" / "python.exe");
            auto programsRoot = localAppData / "Programs" / "Python";
            if (std::filesystem::is_directory(programsRoot, ec)) {
                for (auto const& entry : std::filesystem::directory_iterator(programsRoot, ec)) {
                    if (!entry.is_directory(ec)) continue;
                    candidates.push_back(entry.path() / "python.exe");
                }
            }
        }
        candidates.push_back(searchPath(L"python.exe"));
        std::sort(candidates.begin(), candidates.end(), std::greater<>{});
        return firstExisting(candidates);
    }
#else
    (void)normalized;
#endif
    return {};
}

std::wstring toWide(std::string const& utf8) {
    if (utf8.empty()) return {};
#ifdef _WIN32
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring result((std::size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), result.data(), len);
    return result;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value;
}

bool isShaderSourceExt(std::filesystem::path const& path) {
    auto ext = lower(path.extension().string());
    // .frag/.vert/.comp 是部分原生 GLSL 项目使用的扩展名；漏掉后源码里的 `#define` 不会被收集
    // `#ifndef`，玩家值会被作者的默认值覆盖（"调了没反应"）。
    return ext == ".sc" || ext == ".glsl" || ext == ".sh" || ext == ".h" || ext == ".inc"
        || ext == ".frag" || ext == ".vert" || ext == ".comp";
}

std::vector<std::string> macroNames(std::vector<std::string> const& macros) {
    std::vector<std::string> names;
    for (auto const& macro : macros) {
        auto eq = macro.find('=');
        names.push_back(eq == std::string::npos ? macro : macro.substr(0, eq));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::string join(std::vector<std::string> const& values, char separator) {
    std::string result;
    for (auto const& value : values) {
        if (!result.empty()) result.push_back(separator);
        result += value;
    }
    return result;
}

std::size_t stageCopyTree(
    std::filesystem::path const& source,
    std::filesystem::path const& target,
    std::uint32_t                maxReadableVersion
) {
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (!std::filesystem::is_directory(source, ec)) return 0;

    std::size_t skippedHighVersion = 0;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(
             source,
             std::filesystem::directory_options::skip_permission_denied,
             ec
         )) {
        if (ec) break;
        auto relative = std::filesystem::relative(entry.path(), source, ec);
        if (ec) continue;
        auto destination = target / relative;
        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(destination, ec);
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        std::filesystem::create_directories(destination.parent_path(), ec);
        // 合并源（vanilla/）里的高版本材质会让 lazurite 直接抛异常中止整次编译：
        // 1.26.40 的原版目录里就有 5 个 version 26 的文件
        // （DeferredMixedResolution* / VolumetricsShadows / WaterSimulation），
        // 而当时用的 lazurite 只支持到 25。这些文件本来也不会被着色器包引用，
        // 这里按配置的目标版本上限跳过，避免大型延迟光影包突然整批失败。
        // 26.51 起默认上限就是 26（仓库自带 lazurite-albite-compat，LATEST_VERSION=26），
        // 所以正常情况下这里不会再跳过任何文件。
        if (entry.path().extension() == ".bin") {
            std::uint32_t magic = 0;
            std::uint32_t version = 0;
            if (readMaterialHeader(entry.path(), magic, version)
                && magic == kMaterialMagic && version > maxReadableVersion) {
                ++skippedHighVersion;
                continue;
            }
        }
#ifdef _WIN32
        if (CreateHardLinkW(destination.c_str(), entry.path().c_str(), nullptr)) continue;
#endif
        std::filesystem::copy_file(
            entry.path(),
            destination,
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
    }
    return skippedHighVersion;
}

std::size_t patchStagedSources(std::filesystem::path const& stageDir, std::vector<std::string> const& names) {
    if (names.empty()) return 0;

    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(stageDir, ec)) {
        if (entry.is_regular_file(ec) && isShaderSourceExt(entry.path())) candidates.push_back(entry.path());
    }

    std::size_t patched = 0;
    for (auto const& file : candidates) {
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        in.close();
        if (!patchDefines(content, names)) continue;
        std::filesystem::remove(file, ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) continue;
        out << content;
        ++patched;
    }
    return patched;
}

std::string buildCommandLine(
    CompilerSettings const&         settings,
    std::filesystem::path const&    projectDir,
    std::filesystem::path const&    outputDir,
    CompileOptions const&           options,
    std::string const&              backend
) {
    auto command = backend == "albite" && !settings.lazuriteAlbiteCommand.empty()
                     ? settings.lazuriteAlbiteCommand
                     : settings.lazuriteCommand;
    std::vector<std::string> args = splitCommandPrefix(command);
    if (args.empty()) args.push_back("lazurite");
    if (auto resolved = resolveProgram(args.front()); !resolved.empty()) {
        args.front() = resolved.string();
    }
    args.push_back("build");
    args.push_back(projectDir.string());
    args.push_back("-o");
    args.push_back(outputDir.string());

    if (!options.profiles.empty()) {
        args.push_back("-p");
        args.insert(args.end(), options.profiles.begin(), options.profiles.end());
    }
    if (!options.macros.empty()) {
        args.push_back("-d");
        // lazurite 0.8+ parses a define as "NAME VALUE" (it splits on the first
        // space). The public/UI representation stays NAME=VALUE for settings files.
        for (auto const& macro : options.macros) {
            auto equals = macro.find('=');
            if (equals == std::string::npos) {
                args.push_back(macro);
            } else {
                args.push_back(macro.substr(0, equals) + " " + macro.substr(equals + 1));
            }
        }
    }
    if (!options.materials.empty()) {
        args.push_back("-m");
        args.insert(args.end(), options.materials.begin(), options.materials.end());
    }
    if (settings.maxWorkers > 0) {
        args.push_back("--max-workers");
        args.push_back(std::to_string(settings.maxWorkers));
    }
    if (backend == "albite" && !settings.albitePath.empty()) {
        args.push_back("--albite");
        args.push_back(settings.albitePath);
        if (!settings.shadercPath.empty()) {
            args.push_back("--shaderc");
            args.push_back(settings.shadercPath);
        }
        auto extra = splitCommandPrefix(settings.albiteArgs);
        if (!extra.empty()) {
            args.push_back("--albite-args");
            args.insert(args.end(), extra.begin(), extra.end());
        }
    } else if (!settings.shadercPath.empty()) {
        args.push_back("--shaderc");
        args.push_back(settings.shadercPath);
    }
    if (settings.skipValidation) args.push_back("--skip-validation");

    std::string line;
    for (auto const& arg : args) {
        if (!line.empty()) line.push_back(' ');
        line += quoteArg(arg);
    }
    return line;
}

std::vector<std::filesystem::path> materialFiles(std::filesystem::path const& dir) {
    std::vector<std::filesystem::path> result;
    std::error_code                    ec;
    if (!std::filesystem::is_directory(dir, ec)) return result;
    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename().string().ends_with(".material.bin")) {
            result.push_back(entry.path());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<std::string> sourceMaterialNames(std::filesystem::path const& packDir) {
    std::vector<std::string> names;
    std::error_code          ec;
    auto const               materialsDir = packDir / "materials";
    if (!std::filesystem::is_directory(materialsDir, ec)) return names;
    for (auto const& entry : std::filesystem::directory_iterator(materialsDir, ec)) {
        if (!entry.is_directory(ec)) continue;
        auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '.' || name[0] == '_') continue;
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool seedBuildFromExisting(
    std::filesystem::path const& finalDir,
    std::filesystem::path const& buildDir,
    std::string&                 error
) {
    std::error_code ec;
    if (!std::filesystem::is_directory(finalDir, ec)) return true;
    for (auto const& source : materialFiles(finalDir)) {
        auto destination = buildDir / source.filename();
        ec.clear();
        std::filesystem::copy_file(
            source,
            destination,
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
        if (ec) {
            error = "cannot seed staged build from " + source.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

nlohmann::ordered_json makeMeta(
    CompileOptions const&                    options,
    std::filesystem::path const&             outputDir,
    std::string const&                       backend,
    std::string const&                       compilerVersion,
    std::chrono::milliseconds                elapsed
) {
    nlohmann::ordered_json meta;
    meta["metaVersion"] = 1;
    meta["pack"]        = options.packName;
    meta["preset"]      = options.preset;
    meta["createdUtc"]  = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()
                          )
                              .count();
    meta["settingsHash"] = hashText(join(options.macros, '\n'));
    meta["macros"]       = options.macros;
    meta["profiles"]     = options.profiles;
    meta["platform"]     = options.platform;
    meta["backend"]      = backend;
    meta["compiler"]     = compilerVersion;
    meta["compilerVersion"] = kCompilerCacheVersion;
    meta["complete"]     = options.completeOutput;
    meta["stage"]        = options.stageIndex;
    meta["stageCount"]   = options.stageCount;
    meta["elapsedMs"]    = elapsed.count();
    meta["materialCount"] = 0;
    meta["materials"]     = nlohmann::ordered_json::array();

    for (auto const& file : materialFiles(outputDir)) {
        nlohmann::ordered_json item;
        item["name"] = file.filename().string();
        item["size"] = (std::uint64_t)std::filesystem::file_size(file);
        meta["materials"].push_back(std::move(item));
    }
    meta["materialCount"] = meta["materials"].size();
    return meta;
}

bool writeJsonFile(std::filesystem::path const& file, nlohmann::ordered_json const& value) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << value.dump(2);
    return out.good();
}

bool renameOrCopy(std::filesystem::path const& from, std::filesystem::path const& to, std::string* error) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) return true;

    ec.clear();
    std::filesystem::copy(
        from,
        to,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
        ec
    );
    if (ec) {
        if (error) *error = "cannot move " + from.string() + " -> " + to.string() + ": " + ec.message();
        return false;
    }
    std::filesystem::remove_all(from, ec);
    return true;
}

void pruneBackups(std::filesystem::path const& backupRoot, int keep) {
    if (keep < 0) keep = 0;
    std::error_code ec;
    std::vector<std::filesystem::path> backups;
    for (auto const& entry : std::filesystem::directory_iterator(backupRoot, ec)) {
        if (entry.is_directory(ec)) backups.push_back(entry.path());
    }
    std::sort(backups.begin(), backups.end());
    while ((int)backups.size() > keep) {
        std::filesystem::remove_all(backups.front(), ec);
        backups.erase(backups.begin());
    }
}

bool publishAtomically(
    std::filesystem::path const& buildDir,
    std::filesystem::path const& finalDir,
    nlohmann::ordered_json const& meta,
    int                         keepBackups,
    std::string&                error
) {
    std::error_code ec;
    std::filesystem::create_directories(finalDir.parent_path(), ec);

    if (!writeJsonFile(buildDir / "meta.json", meta)) {
        error = "cannot write meta.json";
        return false;
    }
    if (!validateMaterialDir(buildDir, &error)) return false;

    auto oldDir     = finalDir.parent_path() / (finalDir.filename().string() + ".old");
    auto backupRoot = finalDir / ".backup";
    auto historyDir = finalDir.parent_path() / (finalDir.filename().string() + ".backup-history");
    std::filesystem::remove_all(oldDir, ec);
    std::filesystem::remove_all(historyDir, ec);

    // Keep the previous compiled directory as one flat snapshot. The old
    // implementation moved the whole finalDir (including .backup) under the new
    // .backup, which created recursively nested history directories.
    bool hadHistory = false;
    if (std::filesystem::is_directory(backupRoot, ec)) {
        if (!renameOrCopy(backupRoot, historyDir, &error)) return false;
        hadHistory = true;
    }

    auto restoreHistory = [&] {
        if (!hadHistory) return;
        std::error_code restoreEc;
        std::filesystem::create_directories(finalDir.parent_path(), restoreEc);
        if (!std::filesystem::is_directory(backupRoot, restoreEc)) {
            std::filesystem::rename(historyDir, backupRoot, restoreEc);
        } else {
            for (auto const& child : std::filesystem::directory_iterator(historyDir, restoreEc)) {
                auto destination = backupRoot / child.path().filename();
                std::filesystem::rename(child.path(), destination, restoreEc);
            }
        }
    };

    if (std::filesystem::exists(finalDir, ec)) {
        if (!renameOrCopy(finalDir, oldDir, &error)) {
            restoreHistory();
            return false;
        }
    }
    if (!renameOrCopy(buildDir, finalDir, &error)) {
        if (std::filesystem::exists(oldDir, ec)) {
            std::error_code restoreEc;
            std::filesystem::rename(oldDir, finalDir, restoreEc);
        }
        restoreHistory();
        return false;
    }

    if (std::filesystem::exists(oldDir, ec)) {
        auto stamp     = std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            )
                .count()
        );
        auto backupDir = backupRoot / stamp;
        std::filesystem::create_directories(backupDir.parent_path(), ec);
        if (!renameOrCopy(oldDir, backupDir, &error)) {
            error.clear();
        }
    }

    if (hadHistory && std::filesystem::is_directory(historyDir, ec)) {
        for (auto const& child : std::filesystem::directory_iterator(historyDir, ec)) {
            auto destination = backupRoot / child.path().filename();
            std::filesystem::remove_all(destination, ec);
            renameOrCopy(child.path(), destination, &error);
            error.clear();
        }
        std::filesystem::remove_all(historyDir, ec);
    }

    pruneBackups(backupRoot, keepBackups);
    return true;
}

#ifdef _WIN32
void killProcessTreeById(DWORD pid) {
    if (pid == 0) return;
    auto command = L"taskkill.exe /T /F /PID " + std::to_wstring(pid);

    STARTUPINFOW        startup{};
    startup.cb          = sizeof(startup);
    startup.dwFlags     = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process
        )) {
        return;
    }
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

void drainPipe(HANDLE pipe, std::string& pending, std::function<void(std::string const&)> const& onLine) {
    std::array<char, 4096> buffer{};
    DWORD                  available = 0;
    while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        DWORD read = 0;
        if (!ReadFile(
                pipe,
                buffer.data(),
                (DWORD)std::min<DWORD>(available, (DWORD)buffer.size()),
                &read,
                nullptr
            )
            || read == 0) {
            break;
        }
        pending.append(buffer.data(), read);
        std::size_t position = 0;
        while ((position = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, position);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (onLine) onLine(line);
            pending.erase(0, position + 1);
        }
    }
}
#endif

bool runProcess(
    std::string const&                         commandLine,
    std::filesystem::path const&               workingDir,
    std::function<void(std::string const&)> const& onLine,
    int                                        timeoutSeconds,
    std::function<bool()> const&               isCancelled,
    std::function<void(void*)> const&          onStart,
    unsigned long&                             exitCode,
    bool&                                      cancelled,
    bool&                                      timedOut,
    std::string&                               error
) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{};
    security.nLength        = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // Put the compiler process in a job object so shaderc children are killed
    // together with lazurite on timeout, cancellation, or shutdown.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    STARTUPINFOW startup{};
    startup.cb          = sizeof(startup);
    startup.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput  = writePipe;
    startup.hStdError   = writePipe;
    startup.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    auto                wideLine = toWide(commandLine);
    auto                wideCwd  = workingDir.empty() ? std::wstring{} : workingDir.wstring();
    BOOL                created  = CreateProcessW(
        nullptr,
        wideLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr,
        wideCwd.empty() ? nullptr : wideCwd.c_str(),
        &startup,
        &process
    );
    CloseHandle(writePipe);
    if (!created) {
        if (job != nullptr) CloseHandle(job);
        CloseHandle(readPipe);
        error = "CreateProcess failed (exit " + std::to_string(GetLastError()) + ")";
        return false;
    }

    if (job != nullptr && !AssignProcessToJobObject(job, process.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        TerminateProcess(process.hProcess, 1);
        if (job != nullptr) CloseHandle(job);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readPipe);
        error = "ResumeThread failed";
        return false;
    }

    onStart(process.hProcess);
    std::string pending;
    auto const  started = std::chrono::steady_clock::now();
    auto        terminateTree = [&] {
        if (job != nullptr) TerminateJobObject(job, 1);
        else TerminateProcess(process.hProcess, 1);
        killProcessTreeById(process.dwProcessId);
    };
    while (true) {
        if (isCancelled()) {
            terminateTree();
            cancelled = true;
            break;
        }
        if (timeoutSeconds > 0
            && std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - started
               )
                       .count()
                   >= timeoutSeconds) {
            terminateTree();
            timedOut = true;
            break;
        }

        drainPipe(readPipe, pending, onLine);
        DWORD wait = WaitForSingleObject(process.hProcess, pollMillis);
        if (wait == WAIT_OBJECT_0) break;
    }
    drainPipe(readPipe, pending, onLine);
    if (!pending.empty() && onLine) onLine(pending);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    exitCode = code;
    onStart(nullptr);
    if (job != nullptr) CloseHandle(job); // kills any lingering grandchildren
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    return true;
#else
    (void)commandLine;
    (void)workingDir;
    (void)onLine;
    (void)timeoutSeconds;
    (void)isCancelled;
    (void)onStart;
    (void)exitCode;
    (void)cancelled;
    (void)timedOut;
    error = "subprocess execution is only implemented on Windows";
    return false;
#endif
}

bool applyUniformPatchesWithPython(
    CompilerSettings const&      settings,
    std::filesystem::path const& materialsDir,
    std::vector<UniformPatch> const& patches,
    std::function<void(std::string const&)> const& onLine,
    std::string&                 error
) {
    if (patches.empty()) return true;

    auto patchScript = firstExisting({
        settings.workingDir / "toolchain" / "patch_material_uniform.py",
        settings.workingDir / "tools" / "patch_material_uniform.py",
    });
    if (patchScript.empty()) {
        error = "uniform patcher not found in toolchain/ or tools/";
        return false;
    }

    nlohmann::ordered_json request;
    request["operations"] = nlohmann::ordered_json::array();
    for (auto const& patch : patches) {
        nlohmann::ordered_json operation;
        operation["material"]   = patch.material;
        operation["uniform"]    = patch.uniform;
        operation["components"] = patch.components;
        operation["values"]     = patch.values;
        request["operations"].push_back(std::move(operation));
    }

    auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()
                 )
                     .count();
    auto requestPath = std::filesystem::temp_directory_path()
                     / ("rdl-uniform-patches-" + std::to_string(stamp) + ".json");
    if (!writeJsonFile(requestPath, request)) {
        error = "cannot write uniform patch request: " + requestPath.string();
        return false;
    }

    auto cleanup = [&] {
        std::error_code ec;
        std::filesystem::remove(requestPath, ec);
    };

    std::vector<std::string> args = splitCommandPrefix(settings.lazuriteCommand);
    if (!args.empty() && lower(args.back()).ends_with(".py")) {
        args.back() = patchScript.string();
    } else {
        args = {"py", "-3", patchScript.string()};
    }
    if (auto resolved = resolveProgram(args.front()); !resolved.empty()) {
        args.front() = resolved.string();
    }
    args.push_back("apply-json");
    args.push_back("--directory");
    args.push_back(materialsDir.string());
    args.push_back("--request");
    args.push_back(requestPath.string());

    std::string commandLine;
    for (auto const& arg : args) {
        if (!commandLine.empty()) commandLine.push_back(' ');
        commandLine += quoteArg(arg);
    }

    unsigned long exitCode = 1;
    bool          cancelled = false;
    bool          timedOut = false;
    std::string   runError;
    bool started = runProcess(
        commandLine,
        settings.workingDir,
        onLine,
        (std::min)(settings.timeoutSeconds, 120),
        [] { return false; },
        [](void*) {},
        exitCode,
        cancelled,
        timedOut,
        runError
    );
    cleanup();
    if (!started) {
        error = "cannot start uniform patcher: " + runError;
        return false;
    }
    if (timedOut) {
        error = "uniform patcher timed out";
        return false;
    }
    if (cancelled) {
        error = "uniform patcher cancelled";
        return false;
    }
    if (exitCode != 0) {
        error = "uniform patcher failed (exit " + std::to_string(exitCode) + ")";
        return false;
    }
    return true;
}

bool applyMaterialBlobPatchesWithPython(
    CompilerSettings const&                     settings,
    std::filesystem::path const&                cacheDir,
    std::filesystem::path const&                materialsDir,
    std::vector<std::string> const&             materials,
    std::function<void(std::string const&)> const& onLine,
    std::string&                                error
) {
    if (materials.empty()) return true;

    auto patchScript = firstExisting({
        settings.workingDir / "toolchain" / "patch_material_blobs.py",
        settings.workingDir / "tools" / "patch_material_blobs.py",
    });
    if (patchScript.empty()) {
        error = "material blob patcher not found in toolchain/ or tools/";
        return false;
    }

    std::vector<std::string> args = splitCommandPrefix(settings.lazuriteCommand);
    if (!args.empty() && lower(args.back()).ends_with(".py")) {
        args.back() = patchScript.string();
    } else {
        args = {"py", "-3", patchScript.string()};
    }
    if (auto resolved = resolveProgram(args.front()); !resolved.empty()) {
        args.front() = resolved.string();
    }
    args.push_back("apply-directory");
    args.push_back("--cache-dir");
    args.push_back(cacheDir.string());
    args.push_back("--target-dir");
    args.push_back(materialsDir.string());
    args.insert(args.end(), materials.begin(), materials.end());

    std::string commandLine;
    for (auto const& arg : args) {
        if (!commandLine.empty()) commandLine.push_back(' ');
        commandLine += quoteArg(arg);
    }

    unsigned long exitCode = 1;
    bool          cancelled = false;
    bool          timedOut = false;
    std::string   runError;
    bool started = runProcess(
        commandLine,
        settings.workingDir,
        onLine,
        (std::min)(settings.timeoutSeconds, 120),
        [] { return false; },
        [](void*) {},
        exitCode,
        cancelled,
        timedOut,
        runError
    );
    if (!started) {
        error = "cannot start material blob patcher: " + runError;
        return false;
    }
    if (timedOut) {
        error = "material blob patcher timed out";
        return false;
    }
    if (cancelled) {
        error = "material blob patcher cancelled";
        return false;
    }
    if (exitCode != 0) {
        error = "material blob patcher failed (exit " + std::to_string(exitCode) + ")";
        return false;
    }
    return true;
}

NativeIOIssue parseNativeIOIssue(nlohmann::json const& value) {
    auto text = [&](char const* key) -> std::string {
        if (!value.contains(key) || !value[key].is_string()) return {};
        return value[key].get<std::string>();
    };
    NativeIOIssue item;
    item.kind       = text("kind");
    item.code       = text("code");
    item.material   = text("material");
    item.pass       = text("pass");
    item.buffer     = text("buffer");
    item.message    = text("message");
    item.overrideId = text("overrideId");
    item.reason     = text("reason");
    return item;
}

std::size_t jsonSize(nlohmann::json const& value) {
    if (value.is_number_unsigned()) return value.get<std::size_t>();
    if (value.is_number_integer()) {
        auto const number = value.get<long long>();
        return number > 0 ? static_cast<std::size_t>(number) : 0;
    }
    return 0;
}

bool parseNativeIOReport(
    std::filesystem::path const& reportFile,
    NativeIOReport&              report,
    std::string&                 error
) {
    std::ifstream in(reportFile, std::ios::binary);
    if (!in) {
        error = "native IO validator did not write a JSON report";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        error = "native IO validator wrote an empty JSON report";
        return false;
    }

    try {
        auto const root = nlohmann::json::parse(text);
        if (!root.is_object()) {
            error = "native IO report root must be an object";
            return false;
        }

        NativeIOReport parsed;
        parsed.available = true;
        parsed.json      = text;
        parsed.schemaVersion = root.value("schemaVersion", 0);

        if (root.contains("summary") && root["summary"].is_object()) {
            auto const& summary = root["summary"];
            parsed.candidateMaterials     = jsonSize(summary.value("candidateMaterials", nlohmann::json(0)));
            parsed.engineMaterials        = jsonSize(summary.value("engineMaterials", nlohmann::json(0)));
            parsed.commonMaterials        = jsonSize(summary.value("commonMaterials", nlohmann::json(0)));
            parsed.engineMaterialsUnused  = jsonSize(summary.value("engineMaterialsUnused", nlohmann::json(0)));
            parsed.errors                 = jsonSize(summary.value("errors", nlohmann::json(0)));
            parsed.warnings               = jsonSize(summary.value("warnings", nlohmann::json(0)));
            parsed.overridesApplied       = jsonSize(summary.value("overridesApplied", nlohmann::json(0)));
            parsed.overridesUnused        = jsonSize(summary.value("overridesUnused", nlohmann::json(0)));
        }

        auto appendIssues = [&](char const* key, std::vector<NativeIOIssue>& output) {
            if (!root.contains(key) || !root[key].is_array()) return;
            for (auto const& item : root[key]) {
                if (item.is_object()) output.push_back(parseNativeIOIssue(item));
            }
        };
        appendIssues("issues", parsed.issues);
        appendIssues("overriddenIssues", parsed.overriddenIssues);
        parsed.ok = parsed.errors == 0 && parsed.errorIssueCount() == 0;

        report = std::move(parsed);
        return true;
    } catch (std::exception const& exception) {
        error = "cannot parse native IO JSON report: " + std::string(exception.what());
        return false;
    }
}

NativeIOMappingRecord parseNativeIOMappingRecord(nlohmann::json const& value) {
    auto text = [&](char const* key) -> std::string {
        if (!value.contains(key) || !value[key].is_string()) return {};
        return value[key].get<std::string>();
    };
    NativeIOMappingRecord item;
    item.section = text("section");
    item.id      = text("id");
    item.material = text("material");
    item.pass    = text("pass");
    item.source  = text("source");
    item.target  = text("target");
    item.mode    = text("mode");
    item.reason  = text("reason");
    if (item.source.empty() && value.contains("sourceSemantic")) {
        item.source = text("sourceSemantic");
    }
    if (item.target.empty() && value.contains("targetSemantic")) {
        item.target = text("targetSemantic");
    }
    return item;
}

std::string nativeIOMappingRecordKey(NativeIOMappingRecord const& item) {
    return item.section + '\n' + item.id;
}

void mergeNativeIOMappingRecords(
    std::vector<NativeIOMappingRecord>&       target,
    std::vector<NativeIOMappingRecord> const& incoming
) {
    std::set<std::string> keys;
    for (auto const& item : target) keys.insert(nativeIOMappingRecordKey(item));

    for (auto const& candidate : incoming) {
        if (candidate.section.empty() || candidate.id.empty()) continue;
        auto const key = nativeIOMappingRecordKey(candidate);
        auto const existing = std::find_if(
            target.begin(),
            target.end(),
            [&](NativeIOMappingRecord const& item) {
                return nativeIOMappingRecordKey(item) == key;
            }
        );
        if (existing == target.end()) {
            target.push_back(candidate);
            keys.insert(key);
            continue;
        }

        auto fill = [](std::string& destination, std::string const& source) {
            if (destination.empty()) destination = source;
        };
        fill(existing->material, candidate.material);
        fill(existing->pass, candidate.pass);
        fill(existing->source, candidate.source);
        fill(existing->target, candidate.target);
        fill(existing->mode, candidate.mode);
        fill(existing->reason, candidate.reason);
    }
}

std::filesystem::path nativeIOMappingPath(std::filesystem::path const& packDir);

std::vector<NativeIOMappingRecord> readAttributeMappingsForMaterials(
    std::filesystem::path const& packDir,
    std::filesystem::path const& materialsDir
) {
    std::vector<NativeIOMappingRecord> result;
    std::ifstream in(nativeIOMappingPath(packDir), std::ios::binary);
    if (!in) return result;

    try {
        nlohmann::json root;
        in >> root;
        if (!root.contains("attributes") || !root["attributes"].is_array()) return result;
        for (auto const& value : root["attributes"]) {
            if (!value.is_object()) continue;
            auto item = parseNativeIOMappingRecord(value);
            if (item.section.empty()) item.section = "attributes";
            if (item.id.empty() || item.material.empty()) continue;

            std::error_code ec;
            if (!std::filesystem::is_regular_file(
                    materialsDir / (item.material + ".material.bin"),
                    ec
                )) {
                continue;
            }
            if (item.mode.empty()) {
                auto const varying = packDir / "materials" / item.material / "varying.def.sc";
                item.mode = std::filesystem::is_regular_file(varying, ec)
                              ? "bgfx-varyings"
                              : "albite-define";
            }
            result.push_back(std::move(item));
        }
    } catch (...) {
        // The binary mapping step parses the same document and reports malformed JSON.
    }
    return result;
}

nlohmann::ordered_json nativeIOMappingRecordJson(NativeIOMappingRecord const& item) {
    nlohmann::ordered_json value;
    value["section"]  = item.section;
    value["id"]       = item.id;
    value["material"] = item.material;
    value["pass"]     = item.pass;
    value["source"]   = item.source;
    value["target"]   = item.target;
    value["mode"]     = item.mode;
    value["reason"]   = item.reason;
    return value;
}

bool readNativeIOMappingReport(
    std::filesystem::path const& reportFile,
    std::vector<NativeIOMappingRecord>& applied,
    std::string& error
) {
    std::ifstream in(reportFile, std::ios::binary);
    if (!in) {
        error = "native IO mapping report is missing";
        return false;
    }
    try {
        nlohmann::json root;
        in >> root;
        if (!root.contains("applied") || !root["applied"].is_array()) {
            error = "native IO mapping report has no applied array";
            return false;
        }
        for (auto const& item : root["applied"]) {
            if (item.is_object()) applied.push_back(parseNativeIOMappingRecord(item));
        }
        return true;
    } catch (std::exception const& exception) {
        error = "cannot parse native IO mapping report: " + std::string(exception.what());
        return false;
    }
}

void attachNativeIOMappings(
    NativeIOReport& report,
    std::vector<NativeIOMappingRecord> const& mappings
) {
    report.appliedMappings = mappings;
    try {
        auto root = nlohmann::json::parse(report.json);
        root["appliedMappings"] = nlohmann::json::array();
        for (auto const& item : mappings) {
            nlohmann::json value = nativeIOMappingRecordJson(item);
            root["appliedMappings"].push_back(std::move(value));
        }
        report.json = root.dump(2) + "\n";
    } catch (...) {
        report.appliedMappings.clear();
    }
}

std::vector<NativeIOMappingRecord> readMappingsFromMeta(
    std::filesystem::path const& materialsDir
) {
    std::vector<NativeIOMappingRecord> result;
    std::ifstream in(materialsDir / "meta.json", std::ios::binary);
    if (!in) return result;
    try {
        nlohmann::json meta;
        in >> meta;
        if (!meta.contains("ioMappings") || !meta["ioMappings"].is_array()) return result;
        for (auto const& item : meta["ioMappings"]) {
            if (item.is_object()) result.push_back(parseNativeIOMappingRecord(item));
        }
    } catch (...) {
    }
    return result;
}

std::string firstNativeIOIssue(NativeIOReport const& report) {
    for (auto const& item : report.issues) {
        std::string target = item.material;
        if (!item.pass.empty()) target += "/" + item.pass;
        if (!item.buffer.empty()) target += "/" + item.buffer;
        std::string result = item.kind;
        if (!result.empty()) {
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
        }
        result += " [" + item.code + "] " + target + ": " + item.message;
        return result;
    }
    return {};
}

bool validateNativeIOWithPython(
    CompilerSettings const&                        settings,
    std::filesystem::path const&                   packDir,
    std::filesystem::path const&                   materialsDir,
    std::function<void(std::string const&)> const& onLine,
    NativeIOReport&                                report,
    std::string&                                   error
) {
    auto validationScript = firstExisting({
        settings.workingDir / "toolchain" / "native_io_validate.py",
        settings.workingDir / "tools" / "native_io_validate.py",
    });
    if (validationScript.empty()) {
        error = "native IO validator not found in toolchain/ or tools/";
        return false;
    }

    auto engineContract = firstExisting({
        settings.workingDir / "toolchain" / "compat"
            / "RenderDragon-26.51.vanilla.material-io.json",
        settings.workingDir / "samples" / "compat"
            / "RenderDragon-26.51.vanilla.material-io.json",
    });
    if (engineContract.empty()) {
        error = "native IO engine contract not found";
        return false;
    }
    std::vector<std::string> args = splitCommandPrefix(settings.lazuriteCommand);
    if (!args.empty() && lower(args.back()).ends_with(".py")) {
        args.back() = validationScript.string();
    } else {
        args = {"py", "-3", validationScript.string()};
    }
    if (auto resolved = resolveProgram(args.front()); !resolved.empty()) {
        args.front() = resolved.string();
    }
    args.push_back("--candidate");
    args.push_back(materialsDir.string());
    args.push_back("--engine");
    args.push_back(engineContract.string());
    auto overrides = packDir / "rdloader.io-overrides.json";
    std::error_code ec;
    if (std::filesystem::is_regular_file(overrides, ec)) {
        args.push_back("--overrides");
        args.push_back(overrides.string());
    }
    auto const reportDir = settings.workingDir
        / "data" / "reports" / "compatibility" / ".validation";
    std::filesystem::create_directories(reportDir, ec);
    auto const reportKey = std::hash<std::string>{}(materialsDir.string());
    auto const reportFile = reportDir
        / ("native-io-" + std::to_string(reportKey) + ".json");
    std::filesystem::remove(reportFile, ec);
    args.push_back("--json-out");
    args.push_back(reportFile.string());
    args.push_back("--check");

    std::string commandLine;
    for (auto const& arg : args) {
        if (!commandLine.empty()) commandLine.push_back(' ');
        commandLine += quoteArg(arg);
    }

    std::string firstError;
    auto captureLine = [&](std::string const& line) {
        if (onLine) onLine("[io] " + line);
        if (firstError.empty() && line.starts_with("ERROR ")) {
            firstError = line;
        }
    };

    unsigned long exitCode = 1;
    bool          cancelled = false;
    bool          timedOut = false;
    std::string   runError;
    bool started = runProcess(
        commandLine,
        settings.workingDir,
        captureLine,
        (std::min)(settings.timeoutSeconds, 120),
        [] { return false; },
        [](void*) {},
        exitCode,
        cancelled,
        timedOut,
        runError
    );
    if (!started) {
        error = "cannot start native IO validator: " + runError;
        return false;
    }
    if (timedOut) {
        error = "native IO validation timed out";
        return false;
    }
    if (cancelled) {
        error = "native IO validation cancelled";
        return false;
    }

    std::string parseError;
    bool const parsed = parseNativeIOReport(reportFile, report, parseError);
    std::filesystem::remove(reportFile, ec);

    if (!parsed) {
        error = "native material IO validation failed";
        if (!firstError.empty()) {
            error += ": " + firstError;
        } else if (!runError.empty()) {
            error += ": " + runError;
        } else {
            error += ": " + parseError;
        }
        return false;
    }

    if (exitCode != 0 || !report.ok) {
        error = "native material IO validation failed";
        auto const issue = firstNativeIOIssue(report);
        if (!issue.empty()) {
            error += ": " + issue;
        } else if (!firstError.empty()) {
            error += ": " + firstError;
        }
        return false;
    }
    return true;
}

void mixSignature(std::uint64_t& value, std::string_view text) {
    for (unsigned char c : text) {
        value ^= c;
        value *= 1099511628211ULL;
    }
    value ^= 0xFF;
    value *= 1099511628211ULL;
}

void mixFileSignature(std::uint64_t& value, std::filesystem::path const& path) {
    std::error_code ec;
    mixSignature(value, path.filename().string());
    auto size = std::filesystem::file_size(path, ec);
    mixSignature(value, ec ? "missing" : std::to_string(size));
    auto modified = std::filesystem::last_write_time(path, ec);
    mixSignature(
        value,
        ec ? "missing"
           : std::to_string(modified.time_since_epoch().count())
    );
}

std::filesystem::path nativeIOMappingPath(std::filesystem::path const& packDir) {
    return packDir / "rdloader.io-mappings.json";
}

std::uint64_t nativeIOMappingSignature(std::filesystem::path const& path) {
    std::uint64_t value = 1469598103934665603ULL;
    mixSignature(value, "native-io-mapping-v1");
    mixSignature(value, path.filename().string());
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        mixSignature(value, "missing");
        return value;
    }
    std::array<char, 8192> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto const count = in.gcount();
        if (count <= 0) break;
        mixSignature(value, std::string_view(buffer.data(), static_cast<std::size_t>(count)));
    }
    return value;
}

std::string signatureHex(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

bool nativeIOMappingApplied(
    std::filesystem::path const& materialsDir,
    std::string const&          mappingSignature
) {
    std::ifstream in(materialsDir / "meta.json", std::ios::binary);
    if (!in) return false;
    try {
        nlohmann::json meta;
        in >> meta;
        return meta.value("ioMappingsSignature", std::string{}) == mappingSignature;
    } catch (...) {
        return false;
    }
}

NativeIOReport nativeIOMappingRequiredReport(std::string const& mappingSignature) {
    NativeIOIssue issue;
    issue.kind    = "error";
    issue.code    = "mapping.requires-compilation";
    issue.material = "<pack>";
    issue.message = "pack declares rdloader.io-mappings.json; compile it before loading prebuilt materials";

    nlohmann::json root;
    root["schemaVersion"] = 1;
    root["target"]        = "RenderDragon-26.51";
    root["summary"] = {
        {"candidateMaterials", 0},
        {"engineMaterials", 0},
        {"commonMaterials", 0},
        {"engineMaterialsUnused", 0},
        {"errors", 1},
        {"warnings", 0},
        {"overridesApplied", 0},
        {"overridesUnused", 0},
    };
    root["issues"] = nlohmann::json::array();
    root["issues"].push_back({
        {"kind", issue.kind},
        {"code", issue.code},
        {"material", issue.material},
        {"pass", ""},
        {"buffer", ""},
        {"message", issue.message},
    });
    root["overriddenIssues"]    = nlohmann::json::array();
    root["mappingSignature"]    = mappingSignature;
    root["mappingApplied"]      = false;

    NativeIOReport report;
    report.available      = true;
    report.ok             = false;
    report.schemaVersion  = 1;
    report.errors         = 1;
    report.issues.push_back(std::move(issue));
    report.json           = root.dump(2) + "\n";
    return report;
}

std::uint64_t nativeIOInputSignature(
    std::filesystem::path const& packDir,
    std::filesystem::path const& materialsDir,
    std::filesystem::path const& validationScript,
    std::filesystem::path const& engineContract
) {
    std::uint64_t value = 1469598103934665603ULL;
    mixSignature(value, "native-io-v1");
    mixFileSignature(value, validationScript);
    mixFileSignature(value, engineContract);
    auto mapping = nativeIOMappingPath(packDir);
    std::error_code ec;
    bool const hasMapping = std::filesystem::is_regular_file(mapping, ec);
    mixSignature(value, hasMapping ? "mapping" : "no-mapping");
    if (hasMapping) {
        mixSignature(value, signatureHex(nativeIOMappingSignature(mapping)));
    }
    auto overrides = packDir / "rdloader.io-overrides.json";
    mixSignature(
        value,
        std::filesystem::is_regular_file(overrides, ec) ? "overrides" : "no-overrides"
    );
    if (std::filesystem::is_regular_file(overrides, ec)) mixFileSignature(value, overrides);

    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::directory_iterator(materialsDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto name = entry.path().filename().string();
        if (name.ends_with(".material.bin") || name == "meta.json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (auto const& file : files) mixFileSignature(value, file);
    return value;
}

bool injectWindowsProfile(std::filesystem::path const& stageDir, std::string* error) {
    std::ifstream in(stageDir / "project.json", std::ios::binary);
    if (!in) {
        if (error) *error = "cannot read staged project.json";
        return false;
    }
    try {
        nlohmann::ordered_json project;
        in >> project;
        if (!project.contains("profiles") || !project["profiles"].is_object()) {
            project["profiles"] = nlohmann::ordered_json::object();
        }
        project["profiles"]["rdl_windows"] = {{"platforms", {"Direct3D_SM65"}}};
        std::ofstream out(stageDir / "project.json", std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write staged project.json";
            return false;
        }
        out << project.dump(2);
        return true;
    } catch (...) {
        if (error) *error = "cannot parse staged project.json";
        return false;
    }
}

std::size_t injectAlbiteCompiler(std::filesystem::path const& stageDir, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(stageDir, ec)) {
        if (error) *error = "staged source tree does not exist";
        return 0;
    }

    std::size_t changed = 0;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(
             stageDir,
             std::filesystem::directory_options::skip_permission_denied,
             ec
         )) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.path().filename() != "config.json") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) continue;
        try {
            nlohmann::ordered_json config;
            in >> config;
            config["compiler"]["type"] = "albite";
            std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
            if (!out) continue;
            out << config.dump(2);
            ++changed;
        } catch (...) {
            if (error) *error = "cannot rewrite material config for albite: " + entry.path().string();
            return changed;
        }
    }
    return changed;
}

std::string chooseBackend(CompilerSettings const& settings, CompileOptions const& options) {
    auto backend = options.backend.empty() ? settings.backend : options.backend;
    std::transform(backend.begin(), backend.end(), backend.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    if (backend == "auto") {
        bool const supported = options.platform == "Direct3D_SM60" || options.platform == "Direct3D_SM65"
                            || options.platform == "Metal" || options.platform == "ESSL_310";
        backend = (supported && isExecutableFile(settings.albitePath) && isExecutableFile(settings.shadercPath))
                    ? "albite"
                    : "shaderc";
    }
    if (backend == "albite"
        && (!isExecutableFile(settings.albitePath) || !isExecutableFile(settings.shadercPath))) {
        backend = "shaderc";
    }
    return backend;
}

bool applyNativeIOMappingWithPython(
    CompilerSettings const&                        settings,
    std::filesystem::path const&                   packDir,
    std::filesystem::path const&                   materialsDir,
    std::function<void(std::string const&)> const& onLine,
    std::vector<NativeIOMappingRecord>&            appliedMappings,
    std::string&                                   error
) {
    auto const mapping = nativeIOMappingPath(packDir);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(mapping, ec)) return true;

    auto mappingScript = firstExisting({
        settings.workingDir / "toolchain" / "native_io_map.py",
        settings.workingDir / "tools" / "native_io_map.py",
    });
    if (mappingScript.empty()) {
        error = "native IO mapping tool not found in toolchain/ or tools/";
        return false;
    }
    auto engineContract = firstExisting({
        settings.workingDir / "toolchain" / "compat"
            / "RenderDragon-26.51.vanilla.material-io.json",
        settings.workingDir / "samples" / "compat"
            / "RenderDragon-26.51.vanilla.material-io.json",
    });
    if (engineContract.empty()) {
        error = "native IO engine contract not found";
        return false;
    }
    auto mappingCatalog = firstExisting({
        settings.workingDir / "toolchain" / "compat"
            / "native-io-mapping-catalog.json",
        settings.workingDir / "samples" / "compat"
            / "native-io-mapping-catalog.json",
    });
    if (mappingCatalog.empty()) {
        error = "native IO mapping catalog not found";
        return false;
    }
    if (settings.vanillaDir.empty() || !std::filesystem::is_directory(settings.vanillaDir, ec)) {
        error = "native IO mapping requires the target engine materials directory";
        return false;
    }

    auto const outputDir = materialsDir.parent_path()
        / (materialsDir.filename().string() + ".mapped");
    auto const reportFile = materialsDir.parent_path()
        / (materialsDir.filename().string() + ".mapping-report.json");
    std::filesystem::remove_all(outputDir, ec);
    std::filesystem::remove(reportFile, ec);

    std::vector<std::string> args = splitCommandPrefix(settings.lazuriteCommand);
    if (!args.empty() && lower(args.back()).ends_with(".py")) {
        args.back() = mappingScript.string();
    } else {
        args = {"py", "-3", mappingScript.string()};
    }
    if (auto resolved = resolveProgram(args.front()); !resolved.empty()) {
        args.front() = resolved.string();
    }
    args.push_back("apply");
    args.push_back("--mapping");
    args.push_back(mapping.string());
    args.push_back("--materials");
    args.push_back(materialsDir.string());
    args.push_back("--engine");
    args.push_back(engineContract.string());
    args.push_back("--catalog");
    args.push_back(mappingCatalog.string());
    args.push_back("--engine-materials");
    args.push_back(settings.vanillaDir.string());
    args.push_back("--output");
    args.push_back(outputDir.string());
    args.push_back("--json-out");
    args.push_back(reportFile.string());
    args.push_back("--attributes-applied");

    std::string commandLine;
    for (auto const& arg : args) {
        if (!commandLine.empty()) commandLine.push_back(' ');
        commandLine += quoteArg(arg);
    }

    std::string firstError;
    auto captureLine = [&](std::string const& line) {
        if (onLine) onLine("[mapping] " + line);
        if (firstError.empty()
            && (line.starts_with("ERROR ") || line.starts_with("error:"))) {
            firstError = line;
        }
    };

    unsigned long exitCode = 1;
    bool          cancelled = false;
    bool          timedOut = false;
    std::string   runError;
    bool started = runProcess(
        commandLine,
        settings.workingDir,
        captureLine,
        (std::min)(settings.timeoutSeconds, 120),
        [] { return false; },
        [](void*) {},
        exitCode,
        cancelled,
        timedOut,
        runError
    );
    if (!started) {
        error = "cannot start native IO mapping tool: " + runError;
        return false;
    }
    if (timedOut) {
        std::filesystem::remove_all(outputDir, ec);
        error = "native IO mapping timed out";
        return false;
    }
    if (cancelled) {
        std::filesystem::remove_all(outputDir, ec);
        error = "native IO mapping cancelled";
        return false;
    }
    if (exitCode != 0 || !std::filesystem::is_directory(outputDir, ec)) {
        std::filesystem::remove_all(outputDir, ec);
        error = "native IO mapping failed";
        if (!firstError.empty()) error += ": " + firstError;
        return false;
    }

    if (!readNativeIOMappingReport(reportFile, appliedMappings, error)) {
        std::filesystem::remove_all(outputDir, ec);
        return false;
    }
    std::filesystem::remove_all(materialsDir, ec);
    if (ec) {
        std::filesystem::remove_all(outputDir, ec);
        error = "cannot replace staged materials with mapped output";
        return false;
    }
    std::filesystem::rename(outputDir, materialsDir, ec);
    if (ec) {
        error = "cannot move mapped materials into staged output: " + ec.message();
        return false;
    }
    std::filesystem::remove(reportFile, ec);
    return true;
}

bool applyNativeIOSourceMappingsWithPython(
    CompilerSettings const&                        settings,
    std::filesystem::path const&                   packDir,
    std::filesystem::path const&                   stageDir,
    std::function<void(std::string const&)> const& onLine,
    std::vector<NativeIOMappingRecord>&            appliedMappings,
    std::string&                                   error
) {
    auto const mapping = nativeIOMappingPath(packDir);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(mapping, ec)) return true;

    auto mappingScript = firstExisting({
        settings.workingDir / "toolchain" / "native_io_map.py",
        settings.workingDir / "tools" / "native_io_map.py",
    });
    if (mappingScript.empty()) {
        error = "native IO mapping tool not found in toolchain/ or tools/";
        return false;
    }
    auto engineContract = firstExisting({
        settings.workingDir / "toolchain" / "compat"
            / "RenderDragon-26.51.vanilla.material-io.json",
        settings.workingDir / "samples" / "compat"
            / "RenderDragon-26.51.vanilla.material-io.json",
    });
    if (engineContract.empty()) {
        error = "native IO engine contract not found";
        return false;
    }
    auto mappingCatalog = firstExisting({
        settings.workingDir / "toolchain" / "compat"
            / "native-io-mapping-catalog.json",
        settings.workingDir / "samples" / "compat"
            / "native-io-mapping-catalog.json",
    });
    if (mappingCatalog.empty()) {
        error = "native IO mapping catalog not found";
        return false;
    }

    auto const reportDir = settings.workingDir
        / "data" / "reports" / "compatibility" / ".validation";
    std::filesystem::create_directories(reportDir, ec);
    auto const reportKey = std::hash<std::string>{}(stageDir.string());
    auto const reportFile = reportDir
        / ("native-map-source-" + std::to_string(reportKey) + ".json");
    std::filesystem::remove(reportFile, ec);

    std::vector<std::string> args = splitCommandPrefix(settings.lazuriteCommand);
    if (!args.empty() && lower(args.back()).ends_with(".py")) {
        args.back() = mappingScript.string();
    } else {
        args = {"py", "-3", mappingScript.string()};
    }
    if (auto resolved = resolveProgram(args.front()); !resolved.empty()) {
        args.front() = resolved.string();
    }
    args.push_back("apply-source");
    args.push_back("--mapping");
    args.push_back(mapping.string());
    args.push_back("--source");
    args.push_back(stageDir.string());
    args.push_back("--engine");
    args.push_back(engineContract.string());
    args.push_back("--catalog");
    args.push_back(mappingCatalog.string());
    args.push_back("--json-out");
    args.push_back(reportFile.string());

    std::string commandLine;
    for (auto const& arg : args) {
        if (!commandLine.empty()) commandLine.push_back(' ');
        commandLine += quoteArg(arg);
    }

    std::string firstError;
    auto captureLine = [&](std::string const& line) {
        if (onLine) onLine("[mapping-source] " + line);
        if (firstError.empty()
            && (line.starts_with("ERROR ") || line.starts_with("error:"))) {
            firstError = line;
        }
    };

    unsigned long exitCode = 1;
    bool          cancelled = false;
    bool          timedOut = false;
    std::string   runError;
    bool started = runProcess(
        commandLine,
        settings.workingDir,
        captureLine,
        (std::min)(settings.timeoutSeconds, 120),
        [] { return false; },
        [](void*) {},
        exitCode,
        cancelled,
        timedOut,
        runError
    );
    if (!started) {
        error = "cannot start native IO source mapping: " + runError;
        return false;
    }
    if (timedOut) {
        error = "native IO source mapping timed out";
        return false;
    }
    if (cancelled) {
        error = "native IO source mapping cancelled";
        return false;
    }
    if (exitCode != 0) {
        error = "native IO source mapping failed";
        if (!firstError.empty()) error += ": " + firstError;
        std::filesystem::remove(reportFile, ec);
        return false;
    }
    if (!readNativeIOMappingReport(reportFile, appliedMappings, error)) {
        std::filesystem::remove(reportFile, ec);
        return false;
    }
    std::filesystem::remove(reportFile, ec);
    return true;
}

} // namespace

CompilerService& CompilerService::getInstance() {
    static CompilerService instance;
    return instance;
}

CompilerService::~CompilerService() { shutdown(); }

void CompilerService::configure(CompilerSettings settings) {
    std::lock_guard lock(mMutex);
    mSettings = std::move(settings);
}

void CompilerService::setOnFinished(CompileFinishedCallback callback) {
    std::lock_guard lock(mMutex);
    mOnFinished = std::move(callback);
}

bool CompilerService::compile(
    std::filesystem::path const& packDir,
    std::vector<std::string>     macros,
    std::vector<std::string>     materials,
    std::vector<std::string>     profiles,
    std::filesystem::path const& outputDir
) {
    CompileOptions options;
    options.macros    = std::move(macros);
    options.materials = std::move(materials);
    options.profiles  = std::move(profiles);
    options.outputDir = outputDir;
    options.packName  = packDir.filename().string();
    options.preset    = "default";
    return compile(packDir, std::move(options));
}

bool CompilerService::compile(std::filesystem::path const& packDir, CompileOptions options) {
    bool startWorker = false;
    {
        std::lock_guard lock(mMutex);
        if (mStop) return false;
        if (!mSettings.enableCompilation) {
            mStatus      = CompileStatus::Idle;
            mLastMessage = "compiling is disabled";
            return false;
        }
        if (options.packName.empty()) options.packName = packDir.filename().string();
        if (options.preset.empty()) options.preset = "default";
        if (options.cacheDir.empty()) options.cacheDir = mSettings.outputDir / "cache" / options.packName;

        auto const finalDir = options.outputDir.empty()
                                ? mSettings.outputDir / packDir.filename()
                                : options.outputDir;
        bool queued = false;
        if (options.stagedPublish && !options.force
            && shouldStageColorCompile(options.macros) && !materialFiles(finalDir).empty()) {
            auto allMaterials = options.materials.empty() ? sourceMaterialNames(packDir)
                                                         : options.materials;
            std::vector<std::string> priority;
            std::vector<std::string> background;
            splitStagedMaterials(allMaterials, priority, background);
            if (!priority.empty() && !background.empty()) {
                CompileOptions first = options;
                first.stagedPublish = false;
                first.materials = priority;
                first.keepExistingMaterials = true;
                first.completeOutput = false;
                first.stageIndex = 1;
                first.stageCount = 2;
                if (first.stageSettleMs <= 0) first.stageSettleMs = 2500;

                CompileOptions second = options;
                second.stagedPublish = false;
                second.materials = allMaterials;
                second.keepExistingMaterials = true;
                second.completeOutput = true;
                second.stageIndex = 2;
                second.stageCount = 2;
                second.stageSettleMs = 0;

                mQueue.emplace_back(packDir, std::move(first));
                mQueue.emplace_back(packDir, std::move(second));
                mLastMessage = "queued staged color compile (2 stages)";
                queued = true;
            }
        }
        if (!queued) {
            mQueue.emplace_back(packDir, std::move(options));
            mLastMessage = "queued";
        }
        if (mStatus != CompileStatus::Running) mStatus = CompileStatus::Queued;
        startWorker  = !mThread.joinable();
    }
    if (startWorker) mThread = std::thread([this] { runLoop(); });
    mWake.notify_one();
    return true;
}

void CompilerService::cancel() {
#ifdef _WIN32
    std::vector<HANDLE> handles;
#endif
    {
        std::lock_guard lock(mMutex);
        mCancelRequested = true;
        clearQueue();
        mStatus      = CompileStatus::Cancelled;
        mLastMessage = "compile cancelled";
#ifdef _WIN32
        for (void* handle : mProcessHandles) {
            if (handle != nullptr) handles.push_back((HANDLE)handle);
        }
#endif
    }
#ifdef _WIN32
    for (HANDLE handle : handles) {
        DWORD pid = GetProcessId(handle);
        TerminateProcess(handle, 1);
        if (pid != 0) killProcessTreeById(pid);
    }
#endif
    mWake.notify_one();
}

void CompilerService::clearQueue() {
    if (mQueue.empty()) return;
    mQueue.clear();
    if (mStatus == CompileStatus::Queued) mStatus = CompileStatus::Idle;
}

void CompilerService::runLoop() {
    while (true) {
        std::pair<std::filesystem::path, CompileOptions> job;
        {
            std::unique_lock lock(mMutex);
            mWake.wait(lock, [this] { return mStop || !mQueue.empty(); });
            if (mStop && mQueue.empty()) break;
            job = std::move(mQueue.front());
            mQueue.pop_front();
            mCancelRequested = false;
            mStatus          = CompileStatus::Running;
            mLastMessage     = "starting";
            mLog.clear();
            mLastPackDir = job.first;
        }
        run(std::move(job.first), std::move(job.second));
    }
}

void CompilerService::run(std::filesystem::path packDir, CompileOptions options) {
    auto const started = std::chrono::steady_clock::now();
    CompilerSettings settings;
    {
        std::lock_guard lock(mMutex);
        settings = mSettings;
    }

    packDir                     = absolutePath(packDir);
    settings.workingDir         = absolutePath(settings.workingDir);
    settings.outputDir          = absolutePath(settings.outputDir);
    settings.vanillaDir         = absolutePath(settings.vanillaDir);
    settings.shadercPath        = absolutePath(settings.shadercPath).string();
    // albitePath 默认是相对模组目录的 "toolchain/albite/albite.exe"。
    // 不能直接 absolutePath()：那会按游戏进程的 cwd 解析，实机下会指到游戏根目录。
    if (!settings.albitePath.empty() && std::filesystem::path(settings.albitePath).is_relative()) {
        settings.albitePath = (settings.workingDir / settings.albitePath).lexically_normal().string();
    }
    settings.albitePath         = absolutePath(settings.albitePath).string();
    options.outputDir           = absolutePath(options.outputDir);
    options.cacheDir            = absolutePath(options.cacheDir);

    auto const finalDir = options.outputDir.empty()
                            ? settings.outputDir / packDir.filename()
                            : options.outputDir;
    auto const parent   = finalDir.parent_path();
    auto const buildDir = parent / (finalDir.filename().string() + ".new");
    auto const stageDir = parent / (finalDir.filename().string() + ".src");
    auto const cacheDir = options.cacheDir.empty() ? parent / "cache" : options.cacheDir;

    {
        std::lock_guard lock(mMutex);
        mLastOutputDir = finalDir;
    }

    std::error_code ec;
    std::filesystem::remove_all(buildDir, ec);
    std::filesystem::remove_all(stageDir, ec);
    std::filesystem::create_directories(buildDir, ec);
    if (options.keepExistingMaterials) {
        std::string seedError;
        if (!seedBuildFromExisting(finalDir, buildDir, seedError)) {
            appendLog(seedError);
            CompileFinishedInfo info;
            info.status        = CompileStatus::Failed;
            info.packDir       = packDir;
            info.outputDir     = finalDir;
            info.packName      = options.packName;
            info.preset        = options.preset;
            info.message       = seedError;
            info.stageIndex    = options.stageIndex;
            info.stageCount    = options.stageCount;
            info.completeOutput = options.completeOutput;
            finish(CompileStatus::Failed, seedError, info);
            return;
        }
    }

    auto dependencies = scanMacroDependencies(packDir);
    auto names        = macroNames(options.macros);
    auto backend      = chooseBackend(settings, options);
    if ((options.backend == "albite" || settings.backend == "albite") && backend != "albite") {
        appendLog("albite backend unavailable for this platform/config; using shaderc");
    }
    // 缓存键里的编译器标识。compat 的行为变化（例如 2026-10-17 的
    // uniform_overrides -> uniform 补齐）也必须让旧产物失效，所以带一个版本号。
    auto compilerId   = std::string{kCompilerCacheVersion} + "|" + backend
                      + (settings.preferAlbiteProfileRewrite ? "|rewrite" : "");
    auto const mappingFile = nativeIOMappingPath(packDir);
    std::error_code mappingEc;
    if (std::filesystem::is_regular_file(mappingFile, mappingEc)) {
        compilerId += "|mapapply1:"
                    + signatureHex(nativeIOMappingSignature(mappingFile));
    }

    std::vector<std::string> requested = options.materials;
    if (requested.empty()) {
        requested = dependencies.materials;
    }

    // 增量编译的范围不再靠"依赖图猜受影响材质"来收窄。
    // 旧做法有两个问题：UI 每次都把**整包所有参数**当宏传进来，所以"受影响集合"
    // 恒等于全部材质；而且一旦真的收窄成功，没被重建的材质就不会进 buildDir，
    // 发布时会把它们从 compiled/ 里丢掉。
    // 现在改成：每个材质用"只含它自己引用到的宏"的缓存键去查缓存 —— 命中就复用，
    // 没命中才重建。范围因此天然精确，也不会有丢产物的问题。
    bool const incremental = !dependencies.conservative && !requested.empty();
    std::vector<std::string> toBuild;
    std::string              macroValues = join(options.macros, '\n');

    auto const macrosForMaterial = [&](std::string const& material) -> std::vector<std::string> {
        if (dependencies.conservative || options.macros.empty()) return options.macros;
        auto it = dependencies.materialMacros.find(material);
        // 名单里查不到这个材质、或它被标成 `"*"`（解析失败/未知）时，保守用全集。
        if (it == dependencies.materialMacros.end() || it->second.count("*") != 0) {
            return options.macros;
        }
        std::vector<std::string> subset;
        for (auto const& macro : options.macros) {
            auto equals = macro.find('=');
            auto name   = equals == std::string::npos ? macro : macro.substr(0, equals);
            if (it->second.count(name) != 0) subset.push_back(macro);
        }
        // 交集为空表示"这个材质确实不引用任何可调参数"（只 include bgfx_shader.sh 的材质
        // 只 include bgfx_shader.sh 的材质），不是解析失败 —— 直接返回空集，
        // 让它的缓存键与参数无关，改别的参数时稳定命中。
        // 材质自己的源码仍然参与缓存键（materialSourceFingerprint），改源码照样会重编。
        return subset;
    };

    appendLog(
        "compile scope: " + std::to_string(requested.size()) + " material(s), "
        + std::to_string(names.size()) + " macro(s) requested"
    );

    if (incremental) {
        for (auto const& material : requested) {
            auto keyMacros = macrosForMaterial(material);
            CacheKeyInputs inputs;
            inputs.material          = material;
            inputs.platform          = options.platform;
            inputs.profile           = join(options.profiles, ',');
            inputs.macroValues       = join(keyMacros, '\n');
            inputs.sourceFingerprint = materialSourceFingerprint(packDir, material);
            inputs.compilerVersion   = compilerId;
            appendLog(
                "cache key: " + material + " macros=" + std::to_string(keyMacros.size()) + "/"
                + std::to_string(options.macros.size())
            );
            auto key                 = compileCacheKey(inputs);
            if (!options.force && lookupCompileCache(cacheDir, material, key, buildDir)) {
                appendLog("cache hit: " + material);
            } else {
                toBuild.push_back(material);
            }
        }
    } else {
        toBuild = requested;
    }

    std::vector<NativeIOMappingRecord> appliedMappings;
    if (!toBuild.empty() || !incremental) {
        auto const stageStarted = std::chrono::steady_clock::now();
        // 合并源里高于目标格式版本的材质会被跳过（见 stageCopyTree 注释）。
        auto const maxReadableVersion = static_cast<std::uint32_t>(
            settings.materialVersion > 0 ? settings.materialVersion : static_cast<int>(kMaterialFormatVersion)
        );
        auto const skippedFromPack = stageCopyTree(packDir, stageDir, maxReadableVersion);
        if (skippedFromPack > 0) {
            appendLog(
                "staging: skipped " + std::to_string(skippedFromPack) + " material(s) above format version "
                + std::to_string(maxReadableVersion)
            );
        }
        if (!settings.vanillaDir.empty() && !std::filesystem::is_directory(stageDir / "vanilla", ec)
            && std::filesystem::is_directory(settings.vanillaDir, ec)) {
            // 只暂存"这个包真正要用到"的原版材质。
            //
            // 直接整目录复制会把 1.26.40 的 511 MB（158 个 .material.bin）全塞进编译树，
            // lazurite 每次编译都要全部读一遍 —— 实测 45 秒就花在这里。
            // 着色器包通常只覆盖十几到几十个材质，按名字挑就够了。
            // 名单直接来自"暂存好的包目录"（stageDir/materials），不依赖 requested，
            // 免得它在不同调用路径下是空集或只含增量子集。
            std::vector<std::string> neededMaterials;
            for (auto const& entry : std::filesystem::directory_iterator(stageDir / "materials", ec)) {
                if (!entry.is_directory(ec)) continue;
                auto name = entry.path().filename().string();
                if (name.empty() || name[0] == '.' || name[0] == '_') continue;
                neededMaterials.push_back(std::move(name));
            }
            if (neededMaterials.empty()) neededMaterials = requested; // 兜底

            auto const vanillaMaterialCount = [&] {
                std::size_t count = 0;
                for (auto const& entry : std::filesystem::directory_iterator(settings.vanillaDir, ec)) {
                    if (entry.is_regular_file(ec) && entry.path().extension() == ".bin") ++count;
                }
                return count;
            }();

            std::size_t staged = 0;
            std::size_t skippedHigh = 0;
            if (neededMaterials.empty()
                || (vanillaMaterialCount > 0 && neededMaterials.size() > vanillaMaterialCount)) {
                // 名单不完整就不冒险筛：退回整目录复制，保证能编过。
                appendLog("vanilla merge source: staging whole directory (fallback)");
                auto const skipped = stageCopyTree(settings.vanillaDir, stageDir / "vanilla", maxReadableVersion);
                if (skipped > 0) {
                    appendLog(
                        "vanilla merge source: skipped " + std::to_string(skipped)
                        + " material(s) above format version " + std::to_string(maxReadableVersion)
                    );
                }
            } else {
            for (auto const& material : neededMaterials) {
                auto source = settings.vanillaDir / (material + ".material.bin");
                if (!std::filesystem::is_regular_file(source, ec)) continue;
                std::uint32_t magic = 0;
                std::uint32_t version = 0;
                if (readMaterialHeader(source, magic, version) && magic == kMaterialMagic
                    && version > maxReadableVersion) {
                    ++skippedHigh;
                    continue;
                }
                auto destination = stageDir / "vanilla" / (material + ".material.bin");
                std::filesystem::create_directories(destination.parent_path(), ec);
#ifdef _WIN32
                if (CreateHardLinkW(destination.c_str(), source.c_str(), nullptr)) {
                    ++staged;
                    continue;
                }
#endif
                std::filesystem::copy_file(
                    source,
                    destination,
                    std::filesystem::copy_options::overwrite_existing,
                    ec
                );
                if (!ec) ++staged;
            }
            appendLog(
                "staged " + std::to_string(staged) + " vanilla material(s) of "
                + std::to_string(neededMaterials.size()) + " needed"
            );
            if (skippedHigh > 0) {
                appendLog(
                    "vanilla merge source: skipped " + std::to_string(skippedHigh)
                    + " material(s) above format version " + std::to_string(maxReadableVersion)
                );
            }
            }
        }
        auto const stagedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - stageStarted
        ).count();
        auto patched = patchStagedSources(stageDir, names);
        appendLog(
            "staged source tree in " + std::to_string(stagedMs) + " ms; patched "
            + std::to_string(patched) + " file(s) (" + std::to_string(toBuild.size())
            + " material(s) to build, " + std::to_string(requested.size()) + " requested)"
        );

        std::string sourceMappingError;
        if (!applyNativeIOSourceMappingsWithPython(
                settings,
                packDir,
                stageDir,
                [&](std::string const& line) { appendLog(line); },
                appliedMappings,
                sourceMappingError
            )) {
            appendLog(sourceMappingError);
            std::filesystem::remove_all(buildDir, ec);
            std::filesystem::remove_all(stageDir, ec);
            CompileFinishedInfo info;
            info.status    = CompileStatus::Failed;
            info.packDir   = packDir;
            info.outputDir = finalDir;
            info.packName  = options.packName;
            info.preset    = options.preset;
            info.message   = sourceMappingError;
            finish(CompileStatus::Failed, sourceMappingError, info);
            return;
        }

        CompileOptions buildOptions = options;
        if (backend == "albite") {
            std::string albiteError;
            auto        changed = injectAlbiteCompiler(stageDir, &albiteError);
            if (!albiteError.empty()) {
                appendLog("albite material config rewrite failed: " + albiteError);
            } else {
                appendLog("enabled albite compiler for " + std::to_string(changed) + " material config(s)");
            }
        }
        if (settings.preferAlbiteProfileRewrite && options.platform != "Direct3D_SM65") {
            std::string rewriteError;
            if (injectWindowsProfile(stageDir, &rewriteError)) {
                if (std::find(buildOptions.profiles.begin(), buildOptions.profiles.end(), "rdl_windows")
                    == buildOptions.profiles.end()) {
                    buildOptions.profiles.push_back("rdl_windows");
                }
                appendLog("injected staged rdl_windows profile (Direct3D_SM65 rewrite)");
            } else {
                appendLog("profile rewrite skipped: " + rewriteError);
            }
        }
        std::vector<std::string> selectedMaterials = incremental ? toBuild : options.materials;
        buildOptions.materials.clear();
        for (auto const& material : selectedMaterials) {
            buildOptions.materials.push_back(materialPattern(stageDir, material));
        }

        struct RunResult {
            bool          started   = false;
            unsigned long exitCode  = 1;
            bool          cancelled = false;
            bool          timedOut  = false;
            std::string   error;
        };

        // albite 的材质之间互相独立，参考上游批量编译流程拆成多个 lazurite 进程并行；
        // 每个进程内部仍然用 thread pool 并行编译该材质的变体。
        // RDL_ALBITE_JOBS=<n> 可覆盖并行度，0/缺省 = 自动（最多 8），负数 = 关闭。
        std::size_t jobCount = 1;
        if (backend == "albite" && buildOptions.materials.size() > 1) {
            int envJobs = 0;
            if (char const* env = std::getenv("RDL_ALBITE_JOBS"); env != nullptr && *env != '\0') {
                envJobs = std::atoi(env);
            }
            if (envJobs > 0) {
                jobCount = std::min<std::size_t>((std::size_t)envJobs, buildOptions.materials.size());
            } else if (envJobs < 0) {
                jobCount = 1;
            } else {
                auto const cpu = (std::max)(1u, std::thread::hardware_concurrency());
                jobCount = std::min<std::size_t>(
                    {8u, (std::size_t)cpu, buildOptions.materials.size()}
                );
            }
            jobCount = std::max<std::size_t>(1, jobCount);
        }

        {
            std::lock_guard lock(mMutex);
            mProcessHandles.assign(jobCount, nullptr);
        }

        auto const compilerStarted = std::chrono::steady_clock::now();
        RunResult  primary;

        auto runChunk = [&](std::size_t                     slot,
                            std::filesystem::path const&    outDir,
                            std::vector<std::string> const& materials,
                            int                             innerWorkers) -> RunResult {
            CompilerSettings chunkSettings = settings;
            if (innerWorkers > 0) chunkSettings.maxWorkers = innerWorkers;
            CompileOptions chunkOptions = buildOptions;
            chunkOptions.outputDir = outDir;
            chunkOptions.materials = materials;

            auto line = buildCommandLine(chunkSettings, stageDir, outDir, chunkOptions, backend);
            appendLog("> " + line);

            RunResult result;
            result.started = runProcess(
                line,
                settings.workingDir,
                [this](std::string const& text) { appendLog(text); },
                settings.timeoutSeconds,
                [this] {
                    std::lock_guard lock(mMutex);
                    return mCancelRequested || mStop;
                },
                [this, slot](void* handle) {
                    std::lock_guard lock(mMutex);
                    if (slot < mProcessHandles.size()) mProcessHandles[slot] = handle;
                },
                result.exitCode,
                result.cancelled,
                result.timedOut,
                result.error
            );
            return result;
        };

        if (jobCount <= 1) {
            primary = runChunk(0, buildDir, buildOptions.materials, 0);
        } else {
            appendLog(
                "albite parallel build: " + std::to_string(jobCount) + " process(es) for "
                + std::to_string(buildOptions.materials.size()) + " material(s)"
            );

            std::vector<std::filesystem::path>    chunkDirs(jobCount);
            std::vector<std::vector<std::string>> chunkMaterials(jobCount);

            struct WeightedMaterial {
                std::string   pattern;
                std::uintmax_t weight = 1;
            };
            std::vector<WeightedMaterial> weighted;
            weighted.reserve(buildOptions.materials.size());
            for (auto const& pattern : buildOptions.materials) {
                WeightedMaterial item{pattern, 1};
                std::error_code sizeEc;
                for (auto const& entry :
                     std::filesystem::recursive_directory_iterator(pattern, sizeEc)) {
                    if (sizeEc) break;
                    if (entry.is_regular_file(sizeEc)) {
                        item.weight += entry.file_size(sizeEc);
                    }
                }
                weighted.push_back(std::move(item));
            }
            std::sort(weighted.begin(), weighted.end(), [](auto const& left, auto const& right) {
                return left.weight > right.weight;
            });

            std::vector<std::uintmax_t> chunkLoads(jobCount, 0);
            for (auto const& item : weighted) {
                auto const target = std::min_element(chunkLoads.begin(), chunkLoads.end());
                auto const index  = std::size_t(std::distance(chunkLoads.begin(), target));
                chunkMaterials[index].push_back(item.pattern);
                *target += item.weight;
            }

            auto const cpu   = (std::max)(1u, std::thread::hardware_concurrency());
            int const  inner = settings.maxWorkers > 0
                                 ? settings.maxWorkers
                                 : (std::max)(1, (int)cpu / (int)jobCount);

            std::vector<RunResult>   results(jobCount);
            std::vector<std::thread> threads;
            threads.reserve(jobCount);
            for (std::size_t index = 0; index < jobCount; ++index) {
                chunkDirs[index] = buildDir / (".part" + std::to_string(index));
                std::filesystem::create_directories(chunkDirs[index], ec);
                threads.emplace_back([&, index] {
                    results[index] = runChunk(index, chunkDirs[index], chunkMaterials[index], inner);
                });
            }
            for (auto& thread : threads) thread.join();

            bool allOk = true;
            for (auto const& result : results) {
                if (!result.started || result.cancelled || result.timedOut || result.exitCode != 0) {
                    allOk   = false;
                    primary = result;
                    break;
                }
            }

            if (allOk) {
                for (auto const& dir : chunkDirs) {
                    std::vector<std::filesystem::path> produced;
                    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
                        if (entry.is_regular_file(ec) && entry.path().extension() == ".bin") {
                            produced.push_back(entry.path());
                        }
                    }
                    for (auto const& source : produced) {
                        auto destination = buildDir / source.filename();
                        std::filesystem::remove(destination, ec);
                        ec.clear();
                        std::filesystem::rename(source, destination, ec);
                        if (ec) {
                            ec.clear();
                            std::filesystem::copy_file(
                                source,
                                destination,
                                std::filesystem::copy_options::overwrite_existing,
                                ec
                            );
                        }
                    }
                }
                primary.started  = true;
                primary.exitCode = 0;
            }
            for (auto const& dir : chunkDirs) std::filesystem::remove_all(dir, ec);
        }

        bool const    startedProcess = primary.started;
        bool const    cancelled      = primary.cancelled;
        bool const    timedOut       = primary.timedOut;
        unsigned long exitCode       = primary.exitCode;
        std::string   error          = primary.error;

        auto const compilerMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - compilerStarted
        ).count();
        appendLog(
            "lazurite process took " + std::to_string(compilerMs) + " ms (exit "
            + std::to_string(exitCode) + ")"
            + (jobCount > 1 ? ", parallel " + std::to_string(jobCount) + " process(es)" : "")
        );

        if (!startedProcess || cancelled || timedOut || exitCode != 0) {
            std::string message = error;
            if (cancelled) message = "compile cancelled";
            else if (timedOut) message = "compile timed out";
            else if (exitCode == 9009) {
                message = "compiler command not found (exit 9009); check lazuriteCommand/python installation";
            }
            else if (message.empty()) message = "lazurite exited with code " + std::to_string(exitCode);
            std::filesystem::remove_all(buildDir, ec);
            std::filesystem::remove_all(stageDir, ec);
            CompileFinishedInfo info;
            info.status      = cancelled ? CompileStatus::Cancelled : CompileStatus::Failed;
            info.packDir     = packDir;
            info.outputDir   = finalDir;
            info.packName    = options.packName;
            info.preset      = options.preset;
            info.message     = message;
            finish(info.status, message, info);
            return;
        }
    } else {
        appendLog("all requested materials hit the cache; no compiler process needed");
    }

    std::filesystem::remove_all(stageDir, ec);

    std::string blobPatchError;
    if (!applyMaterialBlobPatchesWithPython(
            settings,
            cacheDir,
            buildDir,
            toBuild,
            [&](std::string const& line) { appendLog(line); },
            blobPatchError
        )) {
        appendLog("blob patch skipped: " + blobPatchError);
    }

    std::string validationError;
    if (!validateMaterialDir(buildDir, &validationError)) {
        std::filesystem::remove_all(buildDir, ec);
        CompileFinishedInfo info;
        info.status    = CompileStatus::Failed;
        info.packDir   = packDir;
        info.outputDir = finalDir;
        info.packName  = options.packName;
        info.preset    = options.preset;
        info.message   = validationError;
        finish(CompileStatus::Failed, validationError, info);
        return;
    }

    // Store cache entries after a successful build. Cached outputs are keyed by
    // material and all inputs that can affect the binary.
    if (incremental) {
        for (auto const& material : toBuild) {
            auto file = buildDir / (material + ".material.bin");
            if (!std::filesystem::is_regular_file(file, ec)) continue;
            CacheKeyInputs inputs;
            inputs.material          = material;
            inputs.platform          = options.platform;
            inputs.profile           = join(options.profiles, ',');
            inputs.macroValues       = join(macrosForMaterial(material), '\n');
            inputs.sourceFingerprint = materialSourceFingerprint(packDir, material);
            inputs.compilerVersion   = compilerId;
            storeCompileCache(cacheDir, material, compileCacheKey(inputs), file);
        }
    }

    std::string patchError;
    if (!applyUniformPatchesWithPython(
            settings,
            buildDir,
            options.uniformPatches,
            [&](std::string const& line) { appendLog("[uniform] " + line); },
            patchError
        )) {
        appendLog(patchError);
        std::filesystem::remove_all(buildDir, ec);
        CompileFinishedInfo info;
        info.status    = CompileStatus::Failed;
        info.packDir   = packDir;
        info.outputDir = finalDir;
        info.packName  = options.packName;
        info.preset    = options.preset;
        info.message   = patchError;
        finish(CompileStatus::Failed, patchError, info);
        return;
    }
    if (!options.uniformPatches.empty()) {
        appendLog(
            "uniform patches applied: " + std::to_string(options.uniformPatches.size())
        );
    }

    std::string mappingError;
    std::vector<NativeIOMappingRecord> binaryMappings;
    if (!applyNativeIOMappingWithPython(
            settings,
            packDir,
            buildDir,
            [&](std::string const& line) { appendLog(line); },
            binaryMappings,
            mappingError
        )) {
        appendLog(mappingError);
        std::filesystem::remove_all(buildDir, ec);
        CompileFinishedInfo info;
        info.status    = CompileStatus::Failed;
        info.packDir   = packDir;
        info.outputDir = finalDir;
        info.packName  = options.packName;
        info.preset    = options.preset;
        info.message   = mappingError;
        finish(CompileStatus::Failed, mappingError, info);
        return;
    }
    mergeNativeIOMappingRecords(appliedMappings, binaryMappings);
    mergeNativeIOMappingRecords(
        appliedMappings,
        readAttributeMappingsForMaterials(packDir, buildDir)
    );
    if (std::filesystem::is_regular_file(nativeIOMappingPath(packDir), ec)) {
        appendLog(
            "native IO mapping records: " + std::to_string(appliedMappings.size())
        );
    }

    NativeIOReport ioReport;
    std::string   ioValidationError;
    if (!validateNativeIOWithPython(
            settings,
            packDir,
            buildDir,
            [&](std::string const& line) { appendLog(line); },
            ioReport,
            ioValidationError
        )) {
        appendLog(ioValidationError);
        std::filesystem::remove_all(buildDir, ec);
        CompileFinishedInfo info;
        info.status    = CompileStatus::Failed;
        info.packDir   = packDir;
        info.outputDir = finalDir;
        info.packName  = options.packName;
        info.preset    = options.preset;
        info.message   = ioValidationError;
        finish(CompileStatus::Failed, ioValidationError, info);
        return;
    }
    appendLog(
        "native material IO validation passed: materials="
        + std::to_string(ioReport.commonMaterials)
        + " errors=" + std::to_string(ioReport.errors)
        + " warnings=" + std::to_string(ioReport.warnings)
        + " overrides=" + std::to_string(ioReport.overridesApplied)
    );

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    auto meta = makeMeta(options, buildDir, backend, "lazurite", elapsed);
    auto const publishedMappingFile = nativeIOMappingPath(packDir);
    std::error_code publishedMappingEc;
    if (std::filesystem::is_regular_file(publishedMappingFile, publishedMappingEc)) {
        meta["ioMappingsSignature"] =
            signatureHex(nativeIOMappingSignature(publishedMappingFile));
        meta["ioMappingsApplied"] = true;
        meta["ioMappings"] = nlohmann::ordered_json::array();
        for (auto const& item : appliedMappings) {
            meta["ioMappings"].push_back(nativeIOMappingRecordJson(item));
        }
    }

    std::string publishError;
    if (!publishAtomically(buildDir, finalDir, meta, settings.keepBackupCount, publishError)) {
        appendLog(publishError);
        std::filesystem::remove_all(buildDir, ec);
        CompileFinishedInfo info;
        info.status    = CompileStatus::Failed;
        info.packDir   = packDir;
        info.outputDir = finalDir;
        info.packName  = options.packName;
        info.preset    = options.preset;
        info.message   = publishError;
        finish(CompileStatus::Failed, publishError, info);
        return;
    }

    auto const materialCount = materialFiles(finalDir).size();
    // 诊断：把宏集合的哈希也写进日志，方便对照"改了参数到底有没有换一份产物"。
    // settingsHash 来自 makeMeta(options,...)，同一组参数重复编译会得到同一个值。
    std::uint64_t outputHash = 1469598103934665603ULL;
    for (auto const& macro : options.macros) {
        for (unsigned char c : macro) {
            outputHash ^= c;
            outputHash *= 1099511628211ULL;
        }
        outputHash ^= '\n';
        outputHash *= 1099511628211ULL;
    }
    appendLog(
        "output fingerprint: macroHash=" + std::to_string(outputHash)
        + " materials=" + std::to_string(materialCount)
    );
    CompileFinishedInfo info;
    info.status        = CompileStatus::Succeeded;
    info.packDir       = packDir;
    info.outputDir     = finalDir;
    info.packName      = options.packName;
    info.preset        = options.preset;
    info.stageIndex    = options.stageIndex;
    info.stageCount    = options.stageCount;
    info.completeOutput = options.completeOutput;
    info.message       = options.stageCount > 1
                           ? "stage " + std::to_string(options.stageIndex) + "/"
                               + std::to_string(options.stageCount) + " published ("
                               + std::to_string(materialCount) + " material(s))"
                           : "build succeeded (" + std::to_string(materialCount) + " material(s))";
    info.materialCount = materialCount;

    if (!options.completeOutput) {
        appendLog(
            "staged publish: visible materials ready; remaining compile continues"
        );
        notifyPublished(info);
        if (options.stageSettleMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(options.stageSettleMs));
        }
        return;
    }
    finish(CompileStatus::Succeeded, info.message, info);
}

void CompilerService::notifyPublished(CompileFinishedInfo info) {
    CompileFinishedCallback callback;
    {
        std::lock_guard lock(mMutex);
        mLastMessage = info.message;
        if (mLog.size() >= maxLogLines) {
            mLog.erase(mLog.begin(), mLog.begin() + (mLog.size() - maxLogLines + 1));
        }
        mLog.push_back(mLastMessage);
        mProcessHandles.clear();
        callback = mOnFinished;
    }
    if (callback) callback(info);
}

void CompilerService::finish(CompileStatus status, std::string message, CompileFinishedInfo info) {
    CompileFinishedCallback callback;
    {
        std::lock_guard lock(mMutex);
        mStatus      = status;
        mLastMessage = std::move(message);
        mLog.push_back(mLastMessage);
        mProcessHandles.clear();
        callback       = mOnFinished;
    }
    if (callback) callback(info);
}

void CompilerService::appendLog(std::string line) {
    std::lock_guard lock(mMutex);
    if (mLog.size() >= maxLogLines) {
        mLog.erase(mLog.begin(), mLog.begin() + (mLog.size() - maxLogLines + 1));
    }
    mLog.push_back(std::move(line));
}

CompileFinishedCallback CompilerService::takeCallback() {
    std::lock_guard lock(mMutex);
    return mOnFinished;
}

CompileStatus CompilerService::status() const {
    std::lock_guard lock(mMutex);
    return mStatus;
}

bool CompilerService::running() const {
    std::lock_guard lock(mMutex);
    return mStatus == CompileStatus::Running || mStatus == CompileStatus::Queued;
}

bool CompilerService::idle() const {
    std::lock_guard lock(mMutex);
    return mStatus == CompileStatus::Idle || mStatus == CompileStatus::Succeeded
        || mStatus == CompileStatus::Failed || mStatus == CompileStatus::Cancelled;
}

std::size_t CompilerService::queuedCount() const {
    std::lock_guard lock(mMutex);
    return mQueue.size();
}

std::string CompilerService::lastMessage() const {
    std::lock_guard lock(mMutex);
    return mLastMessage;
}

void CompilerService::setStatusNotice(std::string message) {
    std::lock_guard lock(mMutex);
    if (mStatus == CompileStatus::Queued || mStatus == CompileStatus::Running) return;
    mLastMessage = std::move(message);
    if (mLog.size() >= maxLogLines) {
        mLog.erase(mLog.begin(), mLog.begin() + (mLog.size() - maxLogLines + 1));
    }
    mLog.push_back(mLastMessage);
}

std::vector<std::string> CompilerService::log() const {
    std::lock_guard lock(mMutex);
    return mLog;
}

std::filesystem::path CompilerService::lastOutputDir() const {
    std::lock_guard lock(mMutex);
    return mLastOutputDir;
}

std::filesystem::path CompilerService::lastPackDir() const {
    std::lock_guard lock(mMutex);
    return mLastPackDir;
}

std::filesystem::path CompilerService::outputRoot() const {
    std::lock_guard lock(mMutex);
    return mSettings.outputDir;
}

bool CompilerService::patchUniforms(
    std::filesystem::path const& materialsDir,
    std::vector<UniformPatch> const& patches,
    std::string& error
) {
    CompilerSettings settings;
    {
        std::lock_guard lock(mMutex);
        settings = mSettings;
    }
    settings.workingDir = absolutePath(settings.workingDir);
    return applyUniformPatchesWithPython(
        settings,
        absolutePath(materialsDir),
        patches,
        {},
        error
    );
}

bool CompilerService::validateNativeIO(
    std::filesystem::path const& packDir,
    std::filesystem::path const& materialsDir,
    NativeIOReport&              report,
    std::string&                 error
) {
    CompilerSettings settings;
    {
        std::lock_guard lock(mMutex);
        settings = mSettings;
    }
    settings.workingDir = absolutePath(settings.workingDir);
    auto const absolutePackDir = absolutePath(packDir);
    auto const absoluteMaterialsDir = absolutePath(materialsDir);

    auto validationScript = firstExisting({
        settings.workingDir / "toolchain" / "native_io_validate.py",
        settings.workingDir / "tools" / "native_io_validate.py",
    });
    auto engineContract = firstExisting({
        settings.workingDir / "toolchain" / "compat"
            / "RenderDragon-26.51.vanilla.material-io.json",
        settings.workingDir / "samples" / "compat"
            / "RenderDragon-26.51.vanilla.material-io.json",
    });
    if (validationScript.empty()) {
        error = "native IO validator not found in toolchain/ or tools/";
        return false;
    }
    if (engineContract.empty()) {
        error = "native IO engine contract not found";
        return false;
    }

    auto const signature = nativeIOInputSignature(
        absolutePackDir,
        absoluteMaterialsDir,
        validationScript,
        engineContract
    );
    auto const cacheKey = absolutePackDir.string() + "|" + absoluteMaterialsDir.string();
    {
        std::lock_guard lock(mMutex);
        auto found = mNativeIOCache.find(cacheKey);
        if (found != mNativeIOCache.end() && found->second.signature == signature) {
            report = found->second.report;
            error = found->second.error;
            return found->second.ok;
        }
    }

    NativeIOReport validationReport;
    std::string    validationError;
    bool           ok = false;
    auto const     mapping = nativeIOMappingPath(absolutePackDir);
    std::error_code mappingEc;
    if (std::filesystem::is_regular_file(mapping, mappingEc)) {
        auto const mappingSignature =
            signatureHex(nativeIOMappingSignature(mapping));
        if (!nativeIOMappingApplied(absoluteMaterialsDir, mappingSignature)) {
            validationReport = nativeIOMappingRequiredReport(mappingSignature);
            validationError =
                "native IO mapping requires a fresh compile before prebuilt materials can load";
        } else {
            ok = validateNativeIOWithPython(
                settings,
                absolutePackDir,
                absoluteMaterialsDir,
                {},
                validationReport,
                validationError
            );
        }
    } else {
        ok = validateNativeIOWithPython(
            settings,
            absolutePackDir,
            absoluteMaterialsDir,
            {},
            validationReport,
            validationError
        );
    }
    auto storedMappings = readMappingsFromMeta(absoluteMaterialsDir);
    if (std::filesystem::is_regular_file(mapping, mappingEc)) {
        auto const mappingSignature = signatureHex(nativeIOMappingSignature(mapping));
        if (nativeIOMappingApplied(absoluteMaterialsDir, mappingSignature)) {
            mergeNativeIOMappingRecords(
                storedMappings,
                readAttributeMappingsForMaterials(absolutePackDir, absoluteMaterialsDir)
            );
        }
    }
    attachNativeIOMappings(validationReport, storedMappings);
    {
        std::lock_guard lock(mMutex);
        mNativeIOCache[cacheKey] = NativeIOCacheEntry{
            .signature = signature,
            .ok        = ok,
            .error     = validationError,
            .report    = validationReport,
        };
    }
    report = std::move(validationReport);
    error = std::move(validationError);
    return ok;
}

void CompilerService::shutdown() {
    {
        std::lock_guard lock(mMutex);
        mStop = true;
        clearQueue();
    }
    mWake.notify_all();
    if (mThread.joinable()) mThread.join();
}

bool compiledOutputNeedsRebuild(std::filesystem::path const& dir) {
    std::ifstream in(dir / "meta.json", std::ios::binary);
    // 作者发行的预编译包没有 meta.json；它们不是我们编译出来的，不该因此重编。
    // 真正没有产物时 validateCompiledOutput 会失败并触发编译。
    if (!in) return false;
    try {
        nlohmann::json meta;
        in >> meta;
        if (meta.value("compilerVersion", std::string{}) != kCompilerCacheVersion) return true;
        return !meta.value("complete", true);
    } catch (...) {
        return true;
    }
}

} // namespace renderdragon_shader_loader
