#pragma once

#include <filesystem>

#include "ll/api/mod/NativeMod.h"

namespace renderdragon_shader_loader {

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

    // 需要支持卸载时再实现该方法。
    // /// @return 卸载成功返回 true。
    // bool unload();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace renderdragon_shader_loader
