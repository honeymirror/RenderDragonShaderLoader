# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 0.2.1

### Fixed

- 修复 Forward 光影在加载前被图形模式预检查错误拒绝的问题。没有灵动视效的设备
  现在可以在 Fancy 模式下加载 NewBx 等前向光影。
- Native IO 预检与加载彻底分离。不兼容结果仍会完整显示和导出，但不再阻止 Apply、
  启动恢复、early redirect 或编译产物发布。
- Python 依赖缺失时，兼容性校验现在会生成结构化报告并给出安装命令，不再只显示
  “没有写出 JSON 报告”。

## 0.2.0-beta

首个公开预发布之后的兼容性与参数完善版本，重点补齐 Native IO 预检、结构化兼容报告、
参数映射、运行时 uniform、失败保护以及配置管理修复。

### Added

- 兼容性预检页新增结构化报告，可滚动查看 error、warning、overridden issue 和映射详情。
- 增加兼容报告导出；编译路径与预编译路径共用同一份报告。
- 支持源码阶段的 attribute 映射，以及二进制阶段的 buffer、pass output 映射和覆盖规则。
- 补齐 attribute、buffer、pass output signature 与 override 的负向测试。
- 增加统一的 Native IO 回归命令，并支持可选的 albite 后端验证。
- 支持 54 材质完整延迟光影包的后端兼容性检查。
- 完善光影自定义参数：支持 `options.json`、分类、显示语言、滑块、开关、枚举、
  编译宏映射和运行时 `vec4` uniform patch。
- 新增 [CUSTOM-PARAMETERS.md](CUSTOM-PARAMETERS.md)，说明参数声明、源码接入、
  中英文名称、运行时 uniform、默认值和发布前检查。
- 新增兼容性报告、配置管理和参数元数据相关回归测试。

### Changed

- Native IO 预检与光影加载彻底分离：不兼容只作为诊断结果，不再阻止 Apply、
  启动恢复、early redirect 或编译产物发布；没有产物、产物过期等结构错误仍会阻止加载。
- 编译与预编译路径不再自动执行完整 IO 预检；只有用户打开兼容性预检或显式运行
  validator 时才做完整检查。映射改写本身仍必须安全通过。
- 缓存命中时继续保留 `meta.json` 中已应用的映射详情。
- Windows 构建使用可复现的 PE 时间戳和稳定的 PDB 名称。
- 配置名称暂时只接受英文 / ASCII 输入；光影参数通过包内语言文件仍可显示中文。

### Fixed

- 修复兼容性页面在“无编译产物”和“无错误项”之间反复变化的问题。
- 修复旧缓存元数据缺少映射数组时无法恢复兼容报告的问题。
- 修复 Python 依赖缺失时兼容页只显示“没有写出 JSON 报告”、无法导出诊断的问题；
  现在会生成结构化 `validator.no-report` 报告并保留具体 Python 错误。
- 修复重置配置后仍显示为 `Custom`、没有回到当前命名配置的问题。
- 修复删除当前配置后参数残留在默认配置中的问题。
- 修复失败校验可能影响 applied 状态或覆盖上一份可用产物的问题。

### Security

- 发布打包会排除 PDB 和日志，并扫描包内文本及二进制中的本机绝对路径。

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

## 0.1.0

### Added

- Initial LeviLamina client mod scaffold for Minecraft Bedrock 1.26.40 (LeviLamina 26.40.x).
- Mod class with `load()` / `enable()` / `disable()` and config loading via `ll::config`.
- `client` variant in `tooth.json` and CI workflows building/publishing the client package.
