# RenderDragonShaderLoader

[English](README_EN.md) | 简体中文

在 Minecraft Bedrock（RenderDragon 渲染管线）里**管理第三方光影包**、**热重载**、
并用 Iris 风格的界面调整光影自定义参数的 **LeviLamina 客户端模组**。

> 目标版本：**Minecraft Bedrock 1.26.51**（客户端）+ **LeviLamina 26.51.7**（Windows x64）
> （材质格式：原版 v26，编译产物也写 v26；读取时兼容 v25 的旧包/旧缓存）
>
> 运行依赖：**Python 3.10–3.14**（推荐 Python 3.12；暂不建议 Python 3.15）
>
> 当前发布：**0.2.1**（兼容报告与加载分离；修复无灵动视效设备上的 Forward 光影加载）
>
> 导入格式：`shaderpacks/` 目录里按 `.zip` 读取；把 `.mcpack` 拖进游戏窗口时会
> **自动改名成 `.zip`** 再导入（`.mcpack` 本身就是一个 zip 容器）。

## Python 运行环境

当前版本使用随模组分发的 Lazurite 脚本进行光影编译和 Native IO 兼容性校验。
支持 **CPython 3.10、3.11、3.12、3.13、3.14**，推荐安装 **Python 3.12 x64**。
**暂不建议 Python 3.15。**

### 为什么暂不建议 Python 3.15

编译和兼容性校验依赖 `pyjson5`，它是带 Cython 扩展的模块：

- Python 3.10–3.14 在 Windows x64 上有可直接安装的预编译 wheel；
- 当前 Python 3.15 没有对应的 `cp315-win_amd64` wheel；
- pip 在 Python 3.15 上会退回到源码编译，需要 Microsoft Visual C++ Build Tools
  和 Windows SDK；
- 即使安装了 Build Tools，Python 3.15 与当前 Cython/`pyjson5` 版本仍可能出现源码兼容
  问题，因此这不是推荐方案。

另外，Windows Python Launcher 的 `py -3` 会选择**已安装的最高 Python 3 版本**。
如果电脑同时有 3.12 和 3.15，那么：

- `py -3` 可能使用 3.15；
- `py -3.12` 才会明确使用 3.12；
- 依赖安装在 3.12，不代表模组使用 `py -3` 时也能找到这些依赖。

### 安装步骤

1. 从 [python.org](https://www.python.org/downloads/windows/) 安装
   **Python 3.12 x64**。安装时勾选 **Python Launcher** 和 **Add Python to PATH**。
2. 完全退出游戏，在 PowerShell 或命令提示符中确认解释器：

   ```powershell
   py -0p
   py -3.12 --version
   ```

3. 进入模组目录，使用明确的 Python 版本安装依赖：

   ```powershell
   cd /d "<版本目录>\mods\RenderDragonShaderLoader"
   py -3.12 -m pip install --only-binary=:all: -r toolchain\requirements.txt
   ```

   `--only-binary=:all:` 会强制使用预编译 wheel，避免 pip 在没有提示的情况下
   下载源码并调用 C++ 编译器。

4. 验证依赖：

   ```powershell
   py -3.12 -c "import pyjson5, Crypto, myers, pcpp, sympy; print('OK')"
   ```

5. 如果电脑还安装了 Python 3.15，编辑模组目录下的 `config/config.json`，
   明确指定 3.12：

   ```json
   "lazuriteCommand": "py -3.12 toolchain/lazurite_albite.py",
   "lazuriteAlbiteCommand": "py -3.12 toolchain/lazurite_albite.py",
   ```

   修改后必须完全退出并重新启动游戏。

PC 上只安装了 3.10–3.14 且 `py -3` 指向其中某个版本时，也可以继续使用默认命令。
如果系统只有 `python.exe`、没有 `py.exe`，请把以下两个命令改成 `python.exe`
的完整路径：

```json
"lazuriteCommand": "\"C:\\Python路径\\python.exe\" toolchain/lazurite_albite.py",
"lazuriteAlbiteCommand": "\"C:\\Python路径\\python.exe\" toolchain/lazurite_albite.py",
```

路径必须替换为本机实际的 `python.exe` 位置，并使用双引号包裹。修改后需要完全重启游戏。

如果坚持使用 Python 3.15，需要安装 Visual Studio Build Tools 的
**“使用 C++ 的桌面开发”**工作负载，包括 MSVC C++ 生成工具和 Windows SDK，
然后改用不带 `--only-binary=:all:` 的命令安装。该路线不受当前版本正式推荐。

## 现在能做什么

### 面板与操作

- 默认按 `G` 打开或关闭光影管理面板。
- 在聊天栏、告示牌、铁砧改名、书、命令方块这些**要打字的界面里，面板热键会让路**：
  敲到绑定的键只会打进输入框，不会弹出面板、也不会被吞掉。
- 面板内可用鼠标浏览光影列表、拖动滚动条，也可把 `.zip` / `.mcpack` 光影包直接拖进游戏窗口完成导入。
- 点击「应用 / 完成」后才会真正启用光影；点击「取消」或按 `Esc` 会丢弃本次未应用的改动。
- 底栏左侧的「设置」按钮打开模组设置页：**改面板热键**（按一个键直接录制）、强制开启灵动视效、
  启动时自动加载上次用的光影、手动覆盖图形模式。
- 方向键可以在内容区、底栏两排按钮、右上角「导入 / 导出 / 重置」与右下角「全透明」之间移动，
  回车等于鼠标左键。

### 光影包识别与导入

- 光影包目录是模组目录下的 `data/shaderpacks/`。
- 通过窗口拖放或导入功能同时接受 `.zip` 和 `.mcpack`；`.mcpack` 会先改名为 `.zip` 再读取。
- `.zip` 会被自动解压到 `data/generated/<包名>/src/` 作为工作副本。
- 已解压的光影目录也可以手工放进 `data/shaderpacks/`，但目录内必须包含 `project.json` 或
  `shaders.properties` 才会计入光影列表。
- 光影包可以自带 `.material.bin` 直接加载，也可以包含源码由模组编译；缺少产物且有可编译源码时会进入编译流程。

### 光影列表与参数界面

- 列表会显示完整包名，滚轮只负责滚动，不会因为滚动而改变当前选中项。
- 启用光影时会根据产物类型自动切换图形模式：延迟产物使用「灵动视效」，前向产物使用「花式」。
  （判定方式是产物文件名里有没有 `Deferred*`，属于启发式；设置页里的「图形模式覆盖」可以手动纠正。）
- 关掉面板 / 退出游戏前用的光影会被记住，下次启动自动挂回去（可在设置页里关掉）。
- 兼容性预检与光影加载相互独立：点击左上角「i」可对当前选中的光影做 Native IO
  预检；即使结果显示“不兼容（仍可加载）”，应用光影也不会被阻止。
- 自定义参数界面采用 Iris 风格：分类页和参数页两层结构，支持两列并排和整行两种版式。
- 参数控件包含滑块、复选框、枚举和颜色；滑块悬停后会展开，滑标可以拖动。
- 参数来自光影包自己的 `options.json` 或 `shaders.properties`；光影包没有可识别声明时会显示空状态。
- 参数声明、源码接入、分类、中文名称和运行时 uniform 的完整写法见
  [`CUSTOM-PARAMETERS.md`](CUSTOM-PARAMETERS.md)。
- 顶栏提供「导入 / 导出 / 重置」，状态提示会在 3 秒后淡出，并带有 Iris 同款点击音效。

### `/shader` 指令

| 指令 | 作用 |
| --- | --- |
| `/shader scan` | 重新扫描 `data/shaderpacks/`，刷新光影包列表。 |
| `/shader ui` | 直接打开或关闭光影管理面板，不依赖快捷键。 |
| `/shader key` | 查看当前打开面板所使用的快捷键。 |
| `/shader key <键名>` | 修改打开面板的快捷键。支持 `A-Z`、`0-9`、`F1-F12`、`SPACE`、`TAB`、`UP` 等名称；`NONE` 表示关闭快捷键。 |
| `/shader vibrant` | 查看「强制开启灵动视效」的状态。 |
| `/shader vibrant <on\|off>` | 打开/关闭强制开启灵动视效（有些服务器会把图形模式锁死，这个开关会绕过判定）。 |
| `/shader list` | 在聊天栏列出已识别的光影包，以及每个包解析到的参数数量。 |
| `/shader rollback` | 关闭材质重定向并回退到原版材质。只有可用的热重载环境才会执行成功。 |

## 编译与产物

- 编译默认开启：调用 `lazurite build` + 随模组分发的 `toolchain/shaderc.exe`，串行任务队列，
  支持取消、超时和日志。
- 参数改动触发材质级增量编译；缓存命中时不启动编译器进程。
- 产物先写到 `compiled.new/`，校验 magic / 目标格式版本 26 / 数量与 `meta.json` 一致后，
  再原子切换到 `compiled/`；旧产物保存到 `compiled/.backup/`。
- 包可以只带预编译 `.material.bin`，也可以带源码让模组编译。编译后由材质热重载挂回游戏。
- 支持 `rdloader.pack.json` / `options.json` / `variants.json` / 包内 `lang/*.lang`，
  并保留普通 `.zip`、`.mcpack` 和 `shaders.properties` 兼容通道。

## 构建

需要：Windows x64 + Python 3.10–3.14 + xmake + Visual Studio（MSVC）+ LLVM（clang-cl）+
LeviLamina 26.51.7 SDK。

```bash
xmake f -y -p windows -a x64 -m release --target_type=client --mods_dir=<游戏 mods 目录>
xmake -y
```

产物是可直接安装的模组包：把 `bin/RenderDragonShaderLoader/` 整个拷进游戏的 `mods/` 目录即可
；如果配置了 `--mods_dir`，构建结束会自动同步到 `mods/RenderDragonShaderLoader/`
（快照保留在 `_backup/`，可用 `tools/sync_mod.ps1 -List/-Rollback` 管理）。

## 目录结构

```
src/            模组源码（不依赖游戏类型，可离线编译/测试）
  mod/          模组类、配置、注册（`/shader` 指令）
  core/         光影包扫描与解析、参数模型、图形模式切换
  hook/         材质热重载（readFile 重定向 + reloadMaterial）、窗口拖放导入
  ui/           界面（UiScreen / UiSettings / UiLayout / UiCanvas）+ 游戏适配层
lang/           文案（zh_CN / en_US）
config/         首次安装的默认配置
samples/        示例光影包（ExamplePack）
```

## 第三方

### 设计与兼容性参考

- [Iris](https://github.com/IrisShaders/Iris)：参考其光影参数界面、参数分类方式、滑块交互和点击音效表现。
- [OptiFine](https://optifine.net/)：参考其 `shaders.properties` 参数表达方式和 `shaderpacks` 目录约定。
- [BetterRenderDragon](https://github.com/QYCottage/BetterRenderDragon)：参考材质热重载思路。
- [Lamium](https://github.com/amatouhake/Lamium)：**本模组 UI 参考了该项目**，包括自绘屏幕和输入接管实现。

以上项目未随本仓库分发第三方二进制或资源文件，仅作为设计与实现参考。

### 编译工具链与随包分发

- [Lazurite](https://github.com/veka0/lazurite)：LGPL-3.0，作为光影工程编译入口。
- [lazurite-albite-compat](https://github.com/bambosan/lazurite-albite-compat)：LGPL-3.0，
  兼容 albite 后端的 Lazurite 源码树，位于 `toolchain/lazurite-albite-compat/`。
- [Albite](https://github.com/bambosan/albite)：MIT License，作为可选的材质编译后端。

- 许可：本项目使用 LGPL-3.0，见 [`LICENSE`](LICENSE)。
- 第三方项目、依赖与分发说明见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
