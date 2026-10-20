#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/NativeIOReport.h"
#include "core/UniformPatch.h"

namespace renderdragon_shader_loader {

// 编译产物格式/兼容层版本。改变 lazurite compat 的行为时 +1，
// 旧的 compiled/ 会被识别为过期并自动重编。
inline constexpr char const* kCompilerCacheVersion = "lazurite|cache2";

struct CompilerSettings {
    std::filesystem::path workingDir;
    std::filesystem::path outputDir;
    std::filesystem::path vanillaDir;

    // 默认走随模组分发的 compat 启动器（lazurite 0.10.0，支持材质格式 22..26）。
    // 系统 pip 里的 lazurite 0.8.4 只到 v25，读 26.51 的原版材质会中止编译。
    std::string lazuriteCommand = "py -3 toolchain/lazurite_albite.py";
    std::string lazuriteAlbiteCommand = "py -3 toolchain/lazurite_albite.py";
    std::string shadercPath;
    std::string backend = "shaderc";
    std::string albitePath;
    std::string albiteArgs;

    bool skipValidation = true;
    bool enableCompilation = true;
    bool preferAlbiteProfileRewrite = false;
    // 目标材质格式版本（1.26.51 = 26 / 1.26.40 = 25）。合并源里高于这个版本的
    // 原版 .material.bin 会在暂存时跳过 —— 因为所用 lazurite 读不了就会整批中止。
    int  materialVersion = 26;
    int  maxWorkers = 0;
    int  timeoutSeconds = 600;
    int  keepBackupCount = 3;
};

enum class CompileStatus { Idle, Queued, Running, Succeeded, Failed, Cancelled };

struct CompileOptions {
    std::vector<std::string> macros;
    std::vector<std::string> materials;
    std::vector<std::string> profiles;
    std::vector<UniformPatch> uniformPatches;
    std::filesystem::path    outputDir;
    std::filesystem::path    cacheDir;
    std::string              packName;
    std::string              preset;
    std::string              platform = "Direct3D_SM65";
    std::string              backend;
    bool                     force = false;
    bool                     writeMeta = true;
    // Two-stage color publish: compile visible materials first, then the
    // background families. Intermediate output is marked incomplete in meta.
    bool                     stagedPublish = false;
    bool                     keepExistingMaterials = false;
    bool                     completeOutput = true;
    int                      stageIndex = 0;
    int                      stageCount = 1;
    int                      stageSettleMs = 0;
};

struct CompileFinishedInfo {
    CompileStatus         status = CompileStatus::Idle;
    std::filesystem::path packDir;
    std::filesystem::path outputDir;
    std::string           packName;
    std::string           preset;
    std::string           message;
    std::size_t           materialCount = 0;
    int                   stageIndex = 0;
    int                   stageCount = 1;
    bool                  completeOutput = true;
};

using CompileFinishedCallback = std::function<void(CompileFinishedInfo const&)>;

// meta.json 里的 compilerVersion 与当前版本不一致（或缺失）时返回 true。
bool compiledOutputNeedsRebuild(std::filesystem::path const& dir);

class CompilerService {
public:
    static CompilerService& getInstance();

    void configure(CompilerSettings settings);
    void setOnFinished(CompileFinishedCallback callback);

    // Compatibility wrapper used by commands and the UI. Returns false only when the
    // job cannot be queued (disabled, shutting down).
    bool compile(
        std::filesystem::path const& packDir,
        std::vector<std::string>     macros = {},
        std::vector<std::string>     materials = {},
        std::vector<std::string>     profiles = {},
        std::filesystem::path const& outputDir = {}
    );

    // Queue a full compile request. The queue is always serial.
    bool compile(std::filesystem::path const& packDir, CompileOptions options);

    // Request cancellation of the active job and all queued jobs.
    void cancel();

    [[nodiscard]] CompileStatus            status() const;
    [[nodiscard]] bool                     running() const;
    [[nodiscard]] bool                     idle() const;
    [[nodiscard]] std::size_t              queuedCount() const;
    [[nodiscard]] std::string              lastMessage() const;
    // 界面状态栏提示（例如“延迟渲染资源不可用，未加载光影”）。编译进行中不会覆盖进度。
    void                                   setStatusNotice(std::string message);
    [[nodiscard]] std::vector<std::string> log() const;
    [[nodiscard]] std::filesystem::path    lastOutputDir() const;
    [[nodiscard]] std::filesystem::path    lastPackDir() const;
    [[nodiscard]] std::filesystem::path    outputRoot() const;

    // Patch existing compiled material defaults without invoking the shader compiler.
    bool patchUniforms(
        std::filesystem::path const& materialsDir,
        std::vector<UniformPatch> const& patches,
        std::string& error
    );

    // Validate an existing native material set against the bundled engine IO
    // contract. Results are cached by material/override/tool mtimes.
    bool validateNativeIO(
        std::filesystem::path const& packDir,
        std::filesystem::path const& materialsDir,
        NativeIOReport&              report,
        std::string& error
    );
    bool validateNativeIO(
        std::filesystem::path const& packDir,
        std::filesystem::path const& materialsDir,
        std::string&                 error
    ) {
        NativeIOReport report;
        return validateNativeIO(packDir, materialsDir, report, error);
    }

    void shutdown();

private:
    CompilerService() = default;
    ~CompilerService();
    CompilerService(CompilerService const&)            = delete;
    CompilerService& operator=(CompilerService const&) = delete;

    void runLoop();
    void run(std::filesystem::path packDir, CompileOptions options);
    void appendLog(std::string line);
    void notifyPublished(CompileFinishedInfo info);
    void finish(CompileStatus status, std::string message, CompileFinishedInfo info);
    CompileFinishedCallback takeCallback();
    void clearQueue();

    mutable std::mutex        mMutex;
    std::condition_variable   mWake;
    CompilerSettings          mSettings;
    CompileStatus             mStatus = CompileStatus::Idle;
    std::vector<std::string>  mLog;
    std::string               mLastMessage;
    std::filesystem::path     mLastOutputDir;
    std::filesystem::path     mLastPackDir;
    CompileFinishedCallback   mOnFinished;
    std::deque<std::pair<std::filesystem::path, CompileOptions>> mQueue;
    std::thread               mThread;
    bool                      mStop = false;
    bool                      mCancelRequested = false;
    struct NativeIOCacheEntry {
        std::uint64_t signature = 0;
        bool          ok        = false;
        std::string   error;
        NativeIOReport report;
    };
    std::unordered_map<std::string, NativeIOCacheEntry> mNativeIOCache;
    // 一次编译可能同时跑多个 lazurite 进程（albite 材质级并行）；
    // cancel() 需要把它们全部杀掉，所以这里存一组句柄。
    std::vector<void*>        mProcessHandles;
};

} // namespace renderdragon_shader_loader
