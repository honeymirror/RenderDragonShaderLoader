#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/ShaderParameter.h"

namespace renderdragon_shader_loader {

// 光影使用的渲染路线。当前按编译产物的文件名判定：
// 出现 *Deferred*.material.bin 视为延迟渲染，否则按前向渲染处理。
enum class ShaderRenderMode { Forward, Deferred };

// 按 *.material.bin 文件名判定渲染路线；目录不存在或无产物时按前向处理。
ShaderRenderMode detectRenderMode(std::filesystem::path const& binsDir);

// 一个着色器包。目录结构（对标 OptiFine/Iris 的 shaderpacks 思路）：
//
//   data/shaderpacks/Mirai.zip            玩家放进来（也支持直接放解压好的目录）
//   data/shaderpacks/Mirai.txt            选项配置（模组读写）
//   data/generated/Mirai/src/             zip 解压出的工作副本（目录包则直接指向原目录）
//   data/generated/Mirai/compiled/        编译产物 *.material.bin（加载就用这里）
//
struct ShaderPack {
    std::string           name;
    std::filesystem::path sourceDir;    // 工作源目录（lazurite 的 project 根）
    std::filesystem::path archivePath;  // 来自哪个 .zip（空 = 目录形式的包）
    std::filesystem::path binsDir;      // 加载/编译目标目录
    std::filesystem::path settingsPath; // <shaderpacks>/<name>.txt
    std::filesystem::path prebuiltDir;  // 包内现成可用的 *.material.bin（空=没有）

    bool                         enabled = true;
    ShaderRenderMode             renderMode = ShaderRenderMode::Forward;
    std::vector<std::string>     profiles;   // shaders.properties 里 profiles=... 声明的 lazurite profile
    std::vector<ShaderParameter> parameters;
};

// 扫描并管理着色器包。
class ShaderPackStore {
public:
    static ShaderPackStore& getInstance();

    std::vector<ShaderPack> const& packs() const { return mPacks; }

    // 按名称查找包；找不到返回 nullptr。
    ShaderPack const* find(std::string_view name) const;

    // 修改某个包某个参数的当前值（UI 用）。成功返回 true。
    bool setParameterValue(std::string_view packName, std::size_t index, std::string value);

    // 扫描 packsRoot（data/shaderpacks/）：
    //   - *.zip       解压到 generatedRoot/<名字>/src/，作为包的源目录
    //   - 子目录      直接当作包（不复制）
    // 产物目录统一是 generatedRoot/<名字>/compiled/。
    void scan(std::filesystem::path const& packsRoot, std::filesystem::path const& generatedRoot);

    // 把某个包的当前选项值写到它的 .txt 配置里。
    static bool saveSettings(ShaderPack const& pack);

private:
    std::vector<ShaderPack> mPacks;

    // containerDir = zip 包的解压根（目录型包传空）。仓库式包（如 mirai-continued-main）里，
    // 发行产物放在 `pack/`，是工程目录 `src/` 的**兄弟**，所以要连同解压根一起找现成产物。
    static ShaderPack                    loadPack(std::filesystem::path const& sourceDir, std::filesystem::path const& packFile,
                                                 std::filesystem::path const& binsDir, std::filesystem::path const& settingsPath,
                                                 std::filesystem::path const& containerDir = {});
    static std::filesystem::path         findPrebuiltMaterials(std::filesystem::path const& packDir);
    static std::vector<ShaderParameter>  parseParameters(std::filesystem::path const& propertiesFile);
    static std::vector<std::string>      parseProfiles(std::filesystem::path const& propertiesFile);
    static void                          applySettings(ShaderPack& pack);
};

} // namespace renderdragon_shader_loader
