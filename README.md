# RenderDragonShaderLoader

[English](README_EN.md) | 简体中文

在 Minecraft Bedrock（RenderDragon 渲染管线）里**管理第三方光影包**、**热重载**、
并用 Iris 风格的界面调整光影自定义参数的 **LeviLamina 客户端模组**。

> 目标版本：**Minecraft Bedrock 1.26.40**（客户端）+ **LeviLamina 26.40.x**（Windows x64）
>
> 当前发布：**0.1.1-beta**（预发布；只包含"光影包管理 + 参数界面"，见下面的"还没打通的"）
>
> 导入格式：`shaderpacks/` 目录里按 `.zip` 读取；把 `.mcpack` 拖进游戏窗口时会
> **自动改名成 `.zip`** 再导入（`.mcpack` 本身就是一个 zip 容器）。

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
- 通过窗口拖放或导入功能添加时，当前只接受 `.zip` 文件；`.mcpack` 会被跳过，不会出现在光影列表中。
- `.zip` 会被自动解压到 `data/generated/<包名>/src/` 作为工作副本。
- 已解压的光影目录也可以手工放进 `data/shaderpacks/`，但目录内必须包含 `project.json` 或
  `shaders.properties` 才会计入光影列表。
- 当前只加载光影包自带的 `.material.bin` 产物，因此一个包能否启用取决于它是否带有已编译产物。

### 光影列表与参数界面

- 列表会显示完整包名，滚轮只负责滚动，不会因为滚动而改变当前选中项。
- 启用光影时会根据产物类型自动切换图形模式：延迟产物使用「灵动视效」，前向产物使用「花式」。
  （判定方式是产物文件名里有没有 `Deferred*`，属于启发式；设置页里的「图形模式覆盖」可以手动纠正。）
- 关掉面板 / 退出游戏前用的光影会被记住，下次启动自动挂回去（可在设置页里关掉）。
- 自定义参数界面采用 Iris 风格：分类页和参数页两层结构，支持两列并排和整行两种版式。
- 参数控件包含滑块、复选框、枚举和颜色；滑块悬停后会展开，滑标可以拖动。
- 参数来自光影包真实声明的 `shaders.properties`；光影包没有声明参数时会显示空状态。
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

## 还没打通的

- **暂不含编译链路**：本模组暂时不会调用 lazurite / shaderc 编译着色器，**只加载光影包自带的
  `.material.bin`**。所以包能不能用，取决于它有没有已经编译好的产物
  （格式见 [`docs/shader-pack-format.md`](docs/shader-pack-format.md)）。自定义参数编译功能仍在开发中。
- 设置页目前只把参数值存进 `data/shaderpacks/<包名>.zip.txt`；**参数要真正影响画面，
  得等编译链路接上**（或包本身就带好对应产物）
- 热重载（`readFile` 重定向 + `reloadMaterial`）代码已就位，但还没在游戏内充分验证

## 构建

需要：Windows x64 + xmake + Visual Studio（MSVC）+ LLVM（clang-cl）+ LeviLamina 26.40.x SDK。

```bash
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -y
```

产物是可直接安装的模组包：把 `bin/RenderDragonShaderLoader/` 整个拷进游戏的 `mods/` 目录即可
（`xmake.lua` 会把 `lang/` 与 `config/config.json` 一起放进产物；构建过程不碰任何游戏目录）。

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
docs/           光影包格式说明
```

## 第三方

- [Iris](https://github.com/IrisShaders/Iris)：参考其光影参数界面、参数分类方式、滑块交互和点击音效表现。
- [OptiFine](https://optifine.net/)：参考其 `shaders.properties` 参数表达方式和 `shaderpacks` 目录约定。
- [BetterRenderDragon](https://github.com/QYCottage/BetterRenderDragon)：参考材质热重载思路。
- [Lamium](https://github.com/amatouhake/Lamium)：**本模组 UI 参考了该项目**，包括自绘屏幕和输入接管实现。

以上项目均未随本仓库分发第三方二进制或资源文件，仅作为设计与实现参考。

- 许可：本项目使用 LGPL-3.0，见 [`LICENSE`](LICENSE)。
- 第三方项目、依赖与分发说明见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
