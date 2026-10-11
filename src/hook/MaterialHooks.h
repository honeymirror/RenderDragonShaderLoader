#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace renderdragon_shader_loader {

// 移植自 BetterRenderDragon 的热重载链路：
//   1. hook 文件读取函数，把 data/renderer/materials/*.material.bin 的内容换成我们编译的产物；
//   2. 在 BgfxFrameBuilder::endFrame 时机调用 reloadMaterial，让游戏重读材质。
// 签名（1.26.40，沿用 BRD 的 1.26.20/1.26.30 模式）已在 exe 上验证唯一命中。
class MaterialHooks {
public:
    static MaterialHooks& getInstance();

    // 解析签名并安装 hook。已有 hook 时直接返回 true。
    bool install();
    void uninstall();

    [[nodiscard]] bool installed() const;
    [[nodiscard]] bool reloadAvailable() const; // 是否定位到了 reloadMaterial
    [[nodiscard]] std::string lastError() const;

    // 设置材质重定向目录（包含 *.material.bin）。传空路径可关闭重定向。
    void setOverrideDir(std::filesystem::path dir);
    [[nodiscard]] std::filesystem::path overrideDir() const;

    // 请求在下一次 endFrame 调用 reloadMaterial，并启用重定向到该目录。
    // 线程安全，供编译线程调用。
    void queueReload(std::filesystem::path outputDir);

    // 回滚：关闭材质重定向并触发一次重载，让游戏回到原始 vanilla 材质。
    // 着色器把画面弄坏 / 一编译就崩时用它兜底。线程安全。
    void restoreVanilla();

    // --- 以下供 hook 内部使用 ---

    // 若 path 命中材质文件且存在重定向产物，则读入 out 并返回 true。
    bool tryOverrideRead(std::string_view path, std::string& out) const;

    // 取出待处理的 reload 请求（并启用重定向到该目录）；无请求返回 false。
    bool consumeReload(std::filesystem::path& out);

private:
    MaterialHooks() = default;
    ~MaterialHooks();
    MaterialHooks(MaterialHooks const&)            = delete;
    MaterialHooks& operator=(MaterialHooks const&) = delete;

    mutable std::mutex    mMutex;
    std::filesystem::path mOverrideDir;
    std::filesystem::path mPendingDir;
    bool                  mRedirectEnabled = false;
    bool                  mReloadPending   = false;
    bool                  mInstalled       = false;
    bool                  mReloadAvailable = false;
    std::string           mLastError;
};

} // namespace renderdragon_shader_loader
