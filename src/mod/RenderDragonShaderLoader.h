#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

#include "ll/api/mod/NativeMod.h"

namespace renderdragon_shader_loader {

struct Config;

class RenderDragonShaderLoader {

public:
    static RenderDragonShaderLoader& getInstance();

    RenderDragonShaderLoader() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    /// LeviLamina 加载模组时调用。只做与游戏无关的初始化（配置、日志、数据库、i18n）。
    /// @return 加载成功返回 true。
    bool load();

    /// LeviLamina 启用模组时调用。
    /// 客户端在“游戏启动完成（主界面出现）”时调用，而不是进入世界时。
    /// @return 启用成功返回 true。
    bool enable();

    /// LeviLamina 禁用模组时调用。
    /// @return 禁用成功返回 true。
    bool disable();

    /// 玩家放包的目录：<modDir>/data/shaderpacks/（*.zip 包 + *.txt 选项配置）。
    [[nodiscard]] std::filesystem::path getPacksDir() const;

    /// 运行时生成目录：<modDir>/data/generated/（解压副本、编译产物等）。
    [[nodiscard]] std::filesystem::path getGeneratedDir() const;

    /// 重新扫描着色器包目录。
    void rescan();

    /// 当前配置（只读）。界面层要读 graphicsModeOverride / autoLoadLastShader 等开关。
    /// 注意不能叫 config()：会遮蔽同名的全局配置对象。
    [[nodiscard]] Config const& getConfig() const;

    /// 改配置并立刻写回 config.json（记住上次用的光影、界面里改开关 / 热键都走它）。
    bool updateConfig(std::function<void(Config&)> const& mutate);

    /// 「应用 / 完成」落地后由界面调用：记住这次真正生效的光影（包名 + 开关）。
    void rememberShaderSelection(std::string const& packName, bool enabled, std::string const& presetName = {});

    /// 启动时按配置恢复上次用的光影（没有配置 / 包不在 / 没有产物时什么都不做）。
    void restoreLastShaderOnStart();

    /// 校验并保存面板热键（设置页录键位 / `/shader key` 共用）。名字不认识返回 false。
    bool setPanelHotkey(std::string const& name);

    /// 触发一次异步编译（编译期宏由包的 shaders.properties 生成）。
    /// @return 已受理返回 true；否则把原因写入 error。
    bool compilePack(std::string const& name, std::string& error);

    /// 回滚到该包上一版编译产物（.backup/），并触发重载。
    /// @return 成功返回 true；否则把原因写入 error。
    bool restorePack(std::string const& name, std::string& error);

    /// 加载某个包的编译产物（shaders/.compiled/<包名>），并触发热重载。
    /// @return 成功返回 true；否则把原因写入 error。
    bool usePack(std::string const& name, std::string& error);
    bool usePackAt(
        std::string const&           name,
        std::filesystem::path const& binsDir,
        std::string&                 error
    );
    bool usePatchedPack(std::string const& name, std::string& error);
    std::filesystem::path createLiveMaterialSnapshot(
        std::filesystem::path const& sourceDir,
        std::string&                 error
    );

    /// 最近一次编译已完成，产物已经热重载生效。
    /// 界面用它显示"编译完成，新着色器已生效"的提示。
    [[nodiscard]] bool restartPending() const { return mRestartPending.load(); }
    void               clearRestartPending() { mRestartPending.store(false); }
    void               markRestartPending() { mRestartPending.store(true); }

    // 需要支持卸载时再实现该方法。
    // /// @return 卸载成功返回 true。
    // bool unload();

private:
    ll::mod::NativeMod& mSelf;
    std::atomic<bool>   mRestartPending{false};
};

} // namespace renderdragon_shader_loader
