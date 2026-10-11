# RenderDragonShaderLoader

在 Minecraft Bedrock（RenderDragon 渲染管线）里**管理第三方光影包**、**热重载**、
并用 Iris 风格的界面调整光影自定义参数的 **LeviLamina 客户端模组**。

> 目标版本：**Minecraft Bedrock 1.26.40**（客户端）+ **LeviLamina 26.40.x**（Windows x64）
>
> 当前发布：**0.1.0-beta**（预发布；只包含"光影包管理 + 参数界面"，见下面的"还没打通的"）

## 现在能做什么

- **面板**：默认热键 `G` 开关（`/shader key <键名>` 可改、`NONE` 关闭；`/shader ui` 也能开关）
- **光影包列表**：扫描 `data/shaderpacks/`（`*.zip` 自动解压，也支持直接放目录）。显示完整文件名、
  滚轮只滚动不改变选中、右侧滚动条可按住拖动；把 `*.zip` 拖进游戏窗口即可导入
- **启用 / 禁用光影**：延迟应用（点「应用 / 完成」才生效，「取消」/ `Esc` 丢弃），
  并按产物类型自动切换图形模式（延迟产物 → 灵动视效，前向产物 → 花式）
- **Iris 风格的自定义参数界面**：分类页 + 参数页两层；两列并排 / 整行两种版式；
  滑块（悬停展开、滑标可拖）、复选框、枚举、颜色；顶栏「导入 / 导出 / 重置」；
  与 Iris 同款的点击音效；状态提示 3 秒后淡出。参数来自光影包**真实声明**的
  `shaders.properties`，包没声明就显示空状态
- **`/shader` 指令**：`scan` / `ui` / `key` / `list` / `rollback`

## 还没打通的

- **不含编译链路**：本模组不会调用 lazurite / shaderc 编译着色器，**只加载光影包自带的
  `.material.bin`**。所以包能不能用，取决于它有没有已经编译好的产物
  （格式见 [`docs/shader-pack-format.md`](docs/shader-pack-format.md)）
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

- 参考实现：[Lamium](https://github.com/amatouhake/Lamium)（自绘屏幕 + 输入接管）、
  BetterRenderDragon（材质热重载思路）。以上**未随本仓库分发**，仅作参考。
- 许可：见 `LICENSE`。
