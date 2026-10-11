# 光影包格式（给包作者）

这个模组只做两件事：**扫描 `data/shaderpacks/` 里的光影包**，以及把包里的
`*.material.bin` 挂到游戏的材质重定向 / 热重载上。**它不编译着色器**（原因见 README 的
"还没打通的"）。所以一个包能不能用，取决于它有没有**已经编译好的** `.material.bin`。

## 目录布局

```
mods/RenderDragonShaderLoader/
  config/config.json           # 模组配置（首次运行自动生成）
  data/
    shaderpacks/               # ← 玩家放光影包的地方
      YourPack.zip             #   *.zip（自动解压）或直接放解压好的目录
      YourPack.zip.txt         #   该包的选项值（模组读写）
    generated/
      YourPack/
        src/                   #   zip 解压出的工作副本
        compiled/              #   实际挂给游戏的 *.material.bin
```

把 `*.zip` 拖进游戏窗口，或者直接丢进 `data/shaderpacks/` 都行；打开面板时会自动重扫。

## 模组怎么在包里找产物

按顺序找第一个命中的目录：

1. `shaders.properties` 里的 `prebuilt = <相对路径>`（显式指定，最保险）；
2. 常见位置：`renderer/materials`、`pack/renderer/materials`、`renderer`、`dist`、`output`；
3. 兜底：在包里浅层递归搜索**含 `*.material.bin` 的目录**。
   - 会跳过 `vanilla/` 和以 `.` / `_` 开头的目录，递归深度上限 5 层；
   - 仓库式布局也认：工程在 `<repo>/src`、发行资源在 `<repo>/pack` 时，
     模组会同时从 zip 解压根和工程目录往下找。

`subpacks/<名字>/renderer/materials/` 里的同名文件会**覆盖**基础目录里的（按名字排序，
名字最后的那个变体生效）—— Mirai 的 `vc`（体积云）子包就是这么用的。

## shaders.properties（可调参数声明）

放在包根目录，用 OptiFine / Iris 风格的子集声明玩家可调的参数。
**没有这个文件也能正常加载**，只是设置页会是空的（会提示"没有声明可调参数"）。

```properties
# 布尔开关：取值留空；开 = 定义宏 NAME=1，关 = 不定义
option.VOLUMETRIC_CLOUDS =

# 滑块：min max [default]
option.SKY_AMBIENT_INTENSITY = 0.0 8.0 4.0

# 枚举：默认取第一个
option.CLOUD_QUALITY = low medium high

# 界面层级与展示顺序（可选）
screen = MAIN
screen.MAIN = ATMOSPHERE CLOUDS
screen.MAIN.ATMOSPHERE = SKY_AMBIENT_INTENSITY
```

- 参数名就是编译宏名；玩家调好的值写在 `data/shaderpacks/<包名>.zip.txt`。
- 旧写法也兼容：`sliders = NAME1 NAME2`、直接 `NAME = ...`。

> 本版本**只保存参数值，不会重新编译着色器** —— 参数真正生效要等编译链路接上（见 README）。

示例包见 `samples/ExamplePack/`。
