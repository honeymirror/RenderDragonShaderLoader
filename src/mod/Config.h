#pragma once

#include <string>

namespace renderdragon_shader_loader {

// LeviLamina 会把 config.json 的内容自动反射到该结构体（ll::config::loadConfig）。
// 注意：结构体必须有 version 字段（用于配置版本/迁移）。
// 新增字段后建议把 version 加一，并在 load() 中按版本做迁移。
struct Config {
    // 注意：新增/改名任何字段都要把这个版本号 +1。否则旧 config.json 会缺少新字段，
    // 反序列化会直接抛错（表现为"模组加载失败"）。
    // 版本 8：新增 albite 专用 lazurite compat 命令。
    // 版本 9：新增 gameVersion / materialVersion（26.51 的材质格式是 v26）。
    int version = 9;
    bool enabled = true;

    // --- 目标游戏 / 材质格式版本 ---
    // 1.26.51 = 原版材质全部是格式版本 26；1.26.40 = 25。
    // materialVersion 决定编译产物写哪个格式版本，也决定合并源里哪些原版材质会被跳过：
    // 高于它的 .material.bin 在暂存时丢弃（所用 lazurite 读不了就会整批中止）。
    // 用仓库自带 lazurite-albite-compat（LATEST_VERSION = 26）时保持 26；
    // 若换回只支持到 25 的 pip lazurite，就把它改成 25。
    std::string gameVersion     = "1.26.51";
    int         materialVersion = 26;

    // --- 目录（相对于模组目录 getModDir()）---
    // 数据根目录。玩家放的东西和运行时生成的东西分开放，对标 OptiFine/Iris：
    //   <dataDir>/shaderpacks/  玩家放置 *.zip 着色器包 + *.txt 选项配置
    //   <dataDir>/generated/    模组运行时产生：解压出的工作副本、编译产物等
    std::string dataDir        = "data";
    std::string shaderPacksDir = "shaderpacks";
    std::string generatedDir   = "generated";
    std::string presetsDir     = "presets";

    std::string vanillaDir = ""; // vanilla 合并源目录，空=自动探测游戏安装目录

    // --- 编译后端 ---
    // 调用 lazurite 的命令前缀。1.26.51 起默认走仓库自带的 compat 启动器
    // （0.10.0，LATEST_VERSION = 26）；系统 pip 里的 0.8.4 只支持到 v25，
    // 读 26.51 的原版材质会直接中止。
    std::string lazuriteCommand = "py -3 toolchain/lazurite_albite.py";
    std::string lazuriteAlbiteCommand = "py -3 toolchain/lazurite_albite.py";
    std::string shadercPath     = "";   // 空则交给 lazurite 在 PATH/工作目录里找
    bool        skipValidation  = true; // 不带 glslang 校验（不打包 glslang 时置 true）

    // 编译后端与任务参数（shaderc 为默认；albite 为可选加速后端）。
    std::string backend                  = "shaderc";
    std::string albitePath               = "toolchain/albite/albite.exe";
    std::string albiteArgs               = "";
    int         maxWorkers               = 0;   // 0 = 交给 lazurite 默认
    // Albite 编译大型延迟光影包较慢：54 材质离线实测 493s，留足余量。
    int         timeoutSeconds           = 1200;
    bool        preferAlbiteProfileRewrite = false;
    bool        enableCompilation        = true;

    // 界面背景压暗档位：0 = 完全不压暗（世界正常透出来，推荐），1 = 全遮住（看不见世界）。
    // 对标 Lamium 的 menuBackground 关/弱/强；占位对话框本身由 BeforeUIRenderEvent 取消，
    // 所以 0 不会露出原版对话框。默认取 0，保证"第一次装上看到的样子"和作者一致。
    double uiBackdrop = 0.0;

    // 打开/关闭面板的热键。可以是 A-Z / 0-9 / F1-F12 / 名字（SPACE、TAB、UP…），
    // 或 "NONE" 表示不绑热键。用 `/shader key <名字>` 改。
    std::string uiKey = "G";

    // --- 游戏兼容 / 行为开关 ---
    // 强制开启「灵动视效」（Vibrant Visuals）：有些服务器会让客户端把图形模式锁在花式，
    // 钩住游戏的判定函数直接置位（BetterRenderDragon 同款做法）。默认开。
    bool forceVibrantVisuals = true;

    // 渲染路线的玩家覆盖：0 = 自动（按产物文件名判定），1 = 强制花式（前向），
    // 2 = 强制灵动视效（延迟）。自动判定只是启发式，名字对不上时用这个兜底。
    int graphicsModeOverride = 0;

    // 启动时自动加载上次用的光影（原版材质 -> 上次的包，不用每次手动点开面板）。
    bool autoLoadLastShader = true;

    // 上次「应用 / 完成」时真正生效的光影：包名（空 = 没用光影）与是否开着。
    std::string lastShaderPack    = "";
    std::string lastShaderPreset  = "";
    bool        lastShaderEnabled = false;
    int         keepBackupCount   = 3;
};

} // namespace renderdragon_shader_loader
