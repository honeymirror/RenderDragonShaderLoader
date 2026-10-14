#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/ShaderParameter.h"

namespace renderdragon_shader_loader {

// 光影使用的渲染路线。
//
// 自动判定看的是产物文件名：`compiled/` 里出现任何一个 `*Deferred*.material.bin`
// 就按延迟渲染（灵动视效）算，否则按前向渲染（花式）算。这只是**启发式**：
// 延迟渲染必须带 `DeferredShading*` / `DeferredIndirectSpecular` / `DeferredWater` 这些
// 延迟 pass 的材质，前向包一般只带 `RenderChunk*` / `*ForwardPBR*` / `*Prepass*`，
// 实测 26.40 的包都分得很干净；但名字终究只是名字，所以玩家可以在设置页里手动覆盖
// （见 Config::graphicsModeOverride）。
enum class ShaderRenderMode { Forward, Deferred };

// 按 *.material.bin 文件名判定渲染路线；目录不存在或无产物时按前向处理。
ShaderRenderMode detectRenderMode(std::filesystem::path const& binsDir);

// 玩家在设置页里的手动覆盖：0 = 自动（用上面那个判定），1 = 强制前向，2 = 强制延迟。
[[nodiscard]] inline ShaderRenderMode applyRenderModeOverride(ShaderRenderMode detected, int overrideMode) {
    if (overrideMode == 1) return ShaderRenderMode::Forward;
    if (overrideMode == 2) return ShaderRenderMode::Deferred;
    return detected;
}

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
