# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 0.2.0-beta

### Added

- Native IO compatibility preflight with structured error, warning, override,
  and mapping reports.
- Source-stage attribute mappings and binary-stage buffer/pass output mappings,
  with atomic validation before publish.
- Compatibility report export and a unified Native IO regression command.
- Optional albite backend validation for a full 54-material deferred pack.

### Changed

- Build metadata is reproducible with deterministic PE timestamps and a stable
  embedded PDB name.
- Configuration names are temporarily limited to English/ASCII input while IME
  handling remains frozen.

### Fixed

- Compatibility page no longer alternates between empty and valid preset output.
- Cached compilations preserve applied mapping details in `meta.json`.
- Existing mapped outputs recover their report even when the old metadata array
  is empty.
- Resetting a configuration reloads the active named preset and no longer shows
  it as `Custom`.
- Deleting the active configuration restores fallback values without polluting
  the fallback settings file.

### Security

- Release packaging excludes PDB files and scans packaged text/binary files for
  local absolute paths before creating an archive.

## 0.1.0

### Added

- Initial LeviLamina client mod scaffold for Minecraft Bedrock 26.40 (LeviLamina 26.40.x).
- Mod class with `load()` / `enable()` / `disable()` and config loading via `ll::config`.
- `client` variant in `tooth.json` and CI workflows building/publishing the client package.

## 0.1.1-beta

首个公开预发布（0.1.0-beta）之后的第一个更新：一批新功能 + 三个实机 bug 的修复。

### Added

- **强制开启「灵动视效」（Vibrant Visuals）**：有些服务器会把图形模式锁死，模组按
  BetterRenderDragon 的做法钩住游戏的判定函数直接置位；设置页可开关，也可用
  `/shader vibrant on|off`。
- **面板底栏左下角「设置」按钮 + 模组设置子页**：改面板热键（按一个键直接录制）、
  强制灵动视效、启动时自动加载上次光影、图形模式覆盖（自动 / 强制花式 / 强制灵动视效）。
- **记住上次用的光影**：关面板 / 退出游戏前生效的那个包会被记下来，下次启动自动挂回去。
- **`.mcpack` 导入**：拖进游戏窗口或直接放进 `data/shaderpacks/` 时自动改名成 `.zip` 再读取。

### Fixed

- 方向键走到不了右下角「全透明」、左下角「设置」和设置页顶栏的四个按钮；另外底栏
  「完成」按上会跳到「取消」而不是正上方的「光影包设置…」——焦点改成按几何位置的走位。
- GUI 标度 100%（GUI 宽度较小）时，底栏按钮会压到左右两个角落按钮上——内容列改成整体
  让开角落按钮的宽度。
- 在聊天栏（以及告示牌 / 铁砧 / 书 / 命令方块）里打字时，敲到面板热键会弹出面板并吞掉
  按键——这些输入界面里热键现在会让路。
- 关闭面板时强制退出「录键位」状态，避免面板热键被锁死。

### Changed

- 热键名 ↔ 虚拟键码的映射抽成 `core/Hotkey`，界面录制与 `/shader key` 共用同一套表；
  配置版本升到 6（新增字段自动迁移）。
- 渲染路线（花式 / 灵动视效）的自动判定仍是按产物文件名的启发式，但加了玩家手动覆盖兜底。
