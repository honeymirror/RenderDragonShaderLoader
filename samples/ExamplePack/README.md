# ExamplePack（模板）

复制整个目录到 `mods/RenderDragonShaderLoader/shaders/` 下，改名为你的包名，
然后放入实际的材质目录（例如 `RenderChunk/`）与 shader 源码。

目录结构参照 lazurite 项目约定：

```
ExamplePack/
  project.json              # 平台 + merge_source
  shaders.properties        # 可调参数声明
  RenderChunk/              # 一个材质（对应 vanilla RenderChunk.material.bin）
    config.json             # 可选：per-material 编译设置
    shaders/
      fragment.sc
      vertex.sc
      varying.def.sc
      bgfx_shader.sh        # BGFX 公共头（每个材质目录自带）
```

`merge_source` 用 `"vanilla"`：包里自带一个 `vanilla/` 子目录放原版材质（推荐，部分仓库式光影包采用这种方式）；
要是包里没有，模组编译前会把游戏安装目录的 `data/renderer/materials` 硬链接进**编译暂存目录**
（不会写进 `shaders/`）。

参数声明、源码宏保护、分类、中文名称和运行时 uniform 的完整写法见
[`../../CUSTOM-PARAMETERS.md`](../../CUSTOM-PARAMETERS.md)。
