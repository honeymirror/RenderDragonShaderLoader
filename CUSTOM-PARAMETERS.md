# 光影自定义参数制作指南

本文档适用于 RenderDragonShaderLoader `0.2.1` 及之后的版本，说明光影作者怎样声明参数，
模组怎样识别这些参数，以及参数最终怎样进入着色器编译或运行时材质。

## 1. 参数文件放在哪里

参数文件必须放在模组实际识别到的光影工程根目录，通常和 `project.json`、`shaders.properties`
或材质目录放在同一层：

```text
你的光影包/
  project.json
  shaders.properties         # 可选，传统参数入口
  options.json               # 可选，推荐的新参数入口
  lang/
    zh_CN.lang
    en_US.lang
  materials/
  ...
```

导入 `.zip` / `.mcpack` 后，模组会先找到真正的工程根目录，再读取参数文件。只有一层普通外壳目录
通常可以被自动识别，但建议作者仍然把 `options.json` 放在工程根目录，避免包里出现多个同名文件时
难以判断实际读取的是哪一个。

模组按以下顺序决定参数来源：

1. `options.json` 能解析出至少一个有效参数时，优先使用 `options.json`。
2. 否则读取 `shaders.properties` 中的 `option.*` 参数。
3. 两者都没有可用参数时，会尝试从源码中的条件编译宏和 `#define` 推断少量开关。

因此不要把一部分参数写在 `options.json`，另一部分依赖同一个包的 `shaders.properties` 作为补充。
`options.json` 一旦有效，传统文件里的参数不会合并进来。

`options.json` 本身能让目录被识别为候选光影包，但它不提供着色器，也不生成 `.material.bin`。
要真正启用画面，包内仍必须有可编译源码，或已经带有可加载的预编译材质。

## 2. 最短写法：使用 shaders.properties

已有的 OptiFine / Iris 风格光影片可以直接使用这一套子集：

```properties
# 布尔开关：空值表示默认关闭
option.VOLUMETRIC_CLOUDS =

# 滑块：最小 最大 默认
option.SKY_AMBIENT_INTENSITY = 0.0 8.0 4.0
option.CLOUD_STEPS = 64 512 200

# 枚举：第一个值是默认值，编译时按位置得到 0、1、2...
option.CLOUD_QUALITY = low medium high

# 强制把某个选项当滑块，范围回退为 0..1、默认 0.5
sliders = EXTRA_STRENGTH

# 当前版本用这些声明调整参数的显示顺序
screen.MAIN.ATMOSPHERE = SKY_AMBIENT_INTENSITY
screen.MAIN.CLOUDS = VOLUMETRIC_CLOUDS CLOUD_QUALITY
```

类型推断规则：

| `option.NAME` 的值 | 控件类型 | 编译行为 |
| --- | --- | --- |
| 空值 | 开关，默认关 | 开启时注入 `NAME=1`，关闭时不定义 |
| `ON OFF`、`true false` 等 | 开关 | 当前值开启时注入 `NAME=1` |
| 两个数字 `min max` | 滑块，默认 `min` | 总是注入 `NAME=当前值` |
| 三个数字 `min max default` | 滑块 | 总是注入 `NAME=当前值` |
| 其它多个文本 | 枚举 | 注入 `NAME=枚举序号` |

注意：`0.2.0-beta` 中 `screen.*` 主要用于参数排序，不会把 `screen.MAIN`
自动变成界面里的分类按钮。需要真正的分类页时，请使用下一节的 `options.json` 和 `group`。

## 3. 推荐写法：使用 options.json

`options.json` 可以统一描述名称、编译宏、分类、默认值、枚举映射、语言键和运行时 uniform。
下面是一份可直接修改的示例：

```json
{
  "options": [
    {
      "name": "VOLUMETRIC_CLOUDS",
      "macro": "VOLUMETRIC_CLOUDS",
      "type": "toggle",
      "default": "OFF",
      "group": "Clouds",
      "langKey": "option.volumetric_clouds"
    },
    {
      "name": "CLOUD_COVERAGE",
      "macro": "CLOUD_COVERAGE",
      "type": "slider",
      "min": 0.0,
      "max": 1.0,
      "default": 0.65,
      "group": "Clouds",
      "langKey": "option.cloud_coverage"
    },
    {
      "name": "CLOUD_QUALITY",
      "macro": "CLOUD_QUALITY",
      "type": "enum",
      "values": ["low", "medium", "high"],
      "macroMap": {
        "low": 0,
        "medium": 1,
        "high": 2
      },
      "default": "medium",
      "group": "Clouds",
      "langKey": "option.cloud_quality"
    },
    {
      "name": "EXPOSURE",
      "kind": "uniform",
      "type": "vec4",
      "control": "slider",
      "material": "*",
      "uniform": "RDL_Exposure",
      "components": [0],
      "min": 0.5,
      "max": 2.0,
      "default": 1.0,
      "group": "Post Processing",
      "langKey": "option.exposure"
    }
  ]
}
```

顶层也支持直接写数组：

```json
[
  {
    "name": "CLOUD_COVERAGE",
    "macro": "CLOUD_COVERAGE",
    "type": "slider",
    "min": 0,
    "max": 1,
    "default": 0.5
  }
]
```

还兼容以宏名为键的紧凑对象写法：

```json
{
  "CLOUD_COVERAGE": {
    "type": "slider",
    "min": 0,
    "max": 1,
    "default": 0.5
  },
  "VOLUMETRIC_CLOUDS": {
    "type": "toggle",
    "default": "OFF"
  }
}
```

正式发布时更推荐使用带 `options` 数组的写法，字段更清楚，也便于以后增加参数。

## 4. options.json 字段

### 通用字段

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `name` | 建议 | 参数在玩家配置文件中使用的稳定名称。省略时回退到 `macro`。发布后不要随意改名。 |
| `macro` | 编译参数必填 | 注入给着色器编译器的宏名。省略时回退到 `name`。 |
| `type` | 建议 | `toggle` / `slider` / `enum` / `select`。运行时 uniform 使用 `vec4`，再由 `control` 指定控件。 |
| `kind` | 否 | 默认是 `source`；运行时 patch 使用 `uniform`。 |
| `default` | 建议 | 默认值。可以是字符串、数字或布尔值。 |
| `min` / `max` | 滑块建议 | 数值范围。未写时分别是 `0` 和 `1`。 |
| `step` | 否 | 当前版本主要用 `step >= 1` 判断是否为整数滑块；浮点滑块请提供合理的 `min/max`，不要依赖任意小数步长。 |
| `group` | 否 | 界面分类名。连续的同一组参数会归入同一页。 |
| `langKey` | 否 | 显示名称对应的语言键，例如 `option.cloud_coverage`；省略时按 `option.<name>` 查找。 |
| `icon` | 否 | 预留字段，当前界面不依赖它。 |

### 枚举字段

```json
{
  "name": "SHADOW_QUALITY",
  "macro": "SHADOW_QUALITY",
  "type": "enum",
  "values": ["off", "low", "high"],
  "macroMap": {
    "off": 0,
    "low": 1,
    "high": 2
  },
  "default": "low"
}
```

- `values` 决定界面中的显示和循环顺序。
- `macroMap` 把玩家看到的文本映射为实际编译数值。
- 只写 `macroMap` 而不写 `values` 也能工作，但映射表会按名称排序，界面顺序不够可控。
- 同时写两者时，`values` 控制顺序，`macroMap` 控制对应数值。

### 运行时 uniform 字段

```json
{
  "name": "EXPOSURE",
  "kind": "uniform",
  "type": "vec4",
  "control": "slider",
  "material": "*",
  "uniform": "RDL_Exposure",
  "components": [0],
  "min": 0.5,
  "max": 2.0,
  "default": 1.0
}
```

| 字段 | 说明 |
| --- | --- |
| `kind` | 必须是 `uniform`。 |
| `type` | 当前运行时 patch 只支持 `vec4`。 |
| `control` | 控件类型：`slider`、`toggle`、`enum` 或 `color`。 |
| `material` | 目标材质名，可写 `ColorPostProcessing`、`ColorPostProcessing.material.bin`，或 `*` 表示所有材质。 |
| `uniform` | 材质里已经存在、带内联默认值的 `vec4` uniform 名称。 |
| `components` | 要修改的分量下标：`0`、`1`、`2`、`3` 分别对应 x、y、z、w。不能重复。 |

运行时 uniform 的规则：

- 目标 uniform 必须已经存在，并且能够写入内联 `vec4` 默认值；模组不会凭空创建 uniform。
- 滑块、开关和枚举会把同一个数值写到 `components` 中列出的所有分量。
- 颜色必须正好提供三个分量，例如 `[0, 1, 2]`，默认值使用 `#RRGGBB`。
- 只改运行时 uniform 且已有可用产物时，模组可以 patch 材质后直接热重载，通常不需要重新编译。
- 如果同时改了源码宏，模组仍会先编译，再应用 uniform patch。
- `mat3`、`mat4`、`external` 虽然能被元数据读取，但当前 patch 路径不支持，不应作为发布参数使用。

`0.2.0-beta` 的颜色控件可以显示当前颜色，但作者侧颜色候选列表还没有完整接入。
如果参数必须在当前版本稳定可调，优先使用滑块、开关或枚举。

## 5. 让源码真的使用参数

参数被界面识别，不等于它会自动改变画面。作者还需要保证编译宏在着色器源码中被读取，并且源码里的
默认定义不会覆盖玩家值。

### 布尔开关

```glsl
#ifdef VOLUMETRIC_CLOUDS
    color = renderVolumetricClouds(color);
#endif
```

模组在开关开启时注入 `VOLUMETRIC_CLOUDS=1`，关闭时不定义它。因此
`#ifdef VOLUMETRIC_CLOUDS` 和 `#if VOLUMETRIC_CLOUDS` 两种写法都能正确工作。

### 数值参数

```glsl
#ifndef CLOUD_COVERAGE
#define CLOUD_COVERAGE 0.65
#endif

float coverage = CLOUD_COVERAGE;
```

必须像上面这样加上 `#ifndef` 保护。若源码无条件写着：

```glsl
#define CLOUD_COVERAGE 0.65
```

它可能覆盖模组注入的值，表现就是“滑条能动，但画面没有变化”。更不要把同一个宏在公共头文件、
`config.h` 或多个材质里重复无条件定义成不同默认值。

### 枚举参数

```glsl
#ifndef CLOUD_QUALITY
#define CLOUD_QUALITY 1
#endif

#if CLOUD_QUALITY == 0
    // low
#elif CLOUD_QUALITY == 1
    // medium
#else
    // high
#endif
```

如果使用 `macroMap`，请让源码中的判断值与映射值完全一致。不要一边映射成 `10/20/30`，
另一边却判断 `0/1/2`。

## 6. 分类与中文名称

`group` 最好连续出现。例如把所有云参数放在一起，再进入后处理参数：

```json
{
  "options": [
    { "name": "CLOUD_A", "macro": "CLOUD_A", "group": "Clouds" },
    { "name": "CLOUD_B", "macro": "CLOUD_B", "group": "Clouds" },
    { "name": "BLOOM", "macro": "BLOOM", "group": "Post Processing" }
  ]
}
```

同一组参数被其它组隔开后，界面可能生成重复的分类页。建议按分类顺序整理 `options.json`。

包内语言文件使用 UTF-8，每行是 `键=值`：

```properties
# lang/zh_CN.lang
group.Clouds=云
option.volumetric_clouds=体积云
option.cloud_coverage=云量
option.cloud_quality=云层质量
group.Post Processing=后处理
option.exposure=曝光
```

```properties
# lang/en_US.lang
group.Clouds=Clouds
option.volumetric_clouds=Volumetric Clouds
option.cloud_coverage=Cloud Coverage
option.cloud_quality=Cloud Quality
group.Post Processing=Post Processing
option.exposure=Exposure
```

模组会优先读取游戏当前语言；找不到时依次回退到 `en_US.lang` 和 `zh_CN.lang`。
如果 `langKey` 是 `option.cloud_coverage`，语言文件里就必须使用完全相同的键。
分类会先查找 `group.<group>`，找不到时再直接使用 `group` 原文。

玩家配置名称目前只接受英文 / ASCII 输入，这个限制不影响光影参数的显示语言。
光影作者可以在 `lang/zh_CN.lang` 中正常使用中文参数名和分类名。

## 7. 默认值、玩家配置与导出

- `options.json` 里的 `default` 是作者默认值。
- 玩家修改后的值保存在所选配置的 `settings.txt` 中。
- 重新打开面板时会用已保存值覆盖作者默认值。
- 「重置」会回到当前配置已保存的值，而不是无条件删除玩家配置。
- 「导出」会写出 `name=value` 形式的文本，后续可以重新导入。
- 如果发布包通过 `rdloader.pack.json` 提供作者预设，作者默认配置通常不应被玩家编辑直接污染。

因此 `name` 是长期兼容键。只调整显示文字时改 `langKey`，不要为了换中文名而修改 `name` 或 `macro`。

## 8. 发布前自检

1. 确认 `options.json` 是有效 JSON，不能有尾逗号、注释或重复键。
2. 确认 `name`、`macro` 和 `uniform` 拼写一致，并且参数名没有重复。
3. 确认每个源码宏只保留一个带 `#ifndef` 保护的默认定义。
4. 打开光影管理面板，确认参数数量和分类正确。
5. 逐个改变参数并应用，确认画面或行为确实变化。
6. 关闭再打开面板，确认值能保存；切换配置后再切回，确认值没有串到其它配置。
7. 使用运行时 uniform 时，先确认包内产物已经包含目标 uniform，再测试只改该参数能否直接热重载。
8. 打包前删除编译日志、临时材质、`compiled.new` 和本机绝对路径。

遇到“参数显示了但修改无效”时，优先检查：

- 源码是否真的读取了这个宏或 uniform。
- 源码是否无条件 `#define` 了同名宏，覆盖了玩家值。
- `macro` 和源码宏名是否完全一致。
- 枚举的 `macroMap` 数值是否和源码判断一致。
- 运行时 uniform 是否确实存在、是否为内联默认值的 `vec4`。
- 改完参数后是否点击了「应用 / 完成」，而不是只拖动滑条就关闭面板。

一个最小但完整的源码参数示例：

```json
{
  "options": [
    {
      "name": "WAVE_STRENGTH",
      "macro": "WAVE_STRENGTH",
      "type": "slider",
      "min": 0.0,
      "max": 2.0,
      "default": 1.0,
      "group": "Water",
      "langKey": "option.wave_strength"
    }
  ]
}
```

```glsl
#ifndef WAVE_STRENGTH
#define WAVE_STRENGTH 1.0
#endif

float wave = sin(time) * WAVE_STRENGTH;
```

```properties
# lang/zh_CN.lang
group.Water=水
option.wave_strength=波浪强度
```

这样模组才能同时完成四件事：识别参数、显示中文名称、保存玩家值，并把新值真正送进着色器。
