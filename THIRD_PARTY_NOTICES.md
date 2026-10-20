# Third-Party Notices / 第三方组件声明

RenderDragonShaderLoader is licensed under the GNU Lesser General Public License v3.0
(LGPL-3.0). See [`LICENSE`](LICENSE).

RenderDragonShaderLoader 使用 GNU Lesser General Public License v3.0（LGPL-3.0）许可，
完整内容见 [`LICENSE`](LICENSE)。

## Referenced Projects / 参考项目

- [Iris](https://github.com/IrisShaders/Iris): LGPL-3.0. Referenced for shader parameter UI
  concepts, categories, slider interaction, and click sound behavior. Not bundled or redistributed.
- [OptiFine](https://optifine.net/): proprietary software. Referenced for `shaders.properties`
  parameter conventions and the `shaderpacks` directory convention. No OptiFine code or assets are
  included.
- [Lamium](https://github.com/amatouhake/Lamium): LGPL-3.0. Referenced for custom-drawn screen and
  input handling implementation ideas. Not bundled or redistributed.
- [BetterRenderDragon](https://github.com/QYCottage/BetterRenderDragon): AGPL-3.0. Referenced for
  material hot reload concepts. Not bundled or redistributed.

以上项目仅作为设计、兼容性或实现思路参考。仓库不分发它们的二进制、贴图或其他资源。

## Build and Runtime Dependencies / 构建与运行依赖

- [LeviLamina](https://github.com/LiteLDev/LeviLamina): LGPL-3.0. Required as the mod runtime
  dependency and declared in [`tooth.json`](tooth.json). Users obtain LeviLamina separately.
- [LeviBuildScript](https://github.com/LiteLDev/LeviBuildScript): build-time dependency. No
  repository license metadata was detected. It is not redistributed by this project.
- [Lazurite](https://github.com/veka0/lazurite): LGPL-3.0. `toolchain/lazurite-albite-compat/`
  contains the albite-compatible source tree. The project modifies its BGFX preprocessing path for
  albite; see the source diff against upstream in `src/lazurite/project/project.py`.
- [Albite](https://github.com/bambosan/albite): MIT License. `toolchain/albite/albite.exe` is the
  official v0.4.0 Windows binary.
- [DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler): University of
  Illinois/NCSA Open Source License. `dxcompiler.dll` and `dxil.dll` are taken from the installed
  Windows SDK x64 redistributable; the DXC license text is included at
  `toolchain/albite/LICENSE-DXC.txt`.

## Third-Party Binaries / 第三方二进制

This repository distributes `toolchain/shaderc.exe`, `toolchain/albite/albite.exe`, and the x64
`dxcompiler.dll` / `dxil.dll` runtime so the default and optional compilation paths work on a fresh
checkout. shaderc is part of the BGFX project; albite is MIT-licensed; DXC/DXIL are distributed
under their upstream notices.

`samples/ExamplePack/include/bgfx/bgfx_shader.sh` is the BGFX shader support header, used only to
build the example pack; it retains its upstream copyright and license notice.

本仓库随包分发 `toolchain/shaderc.exe`（BGFX）、`toolchain/albite`（MIT + DXC 运行库）和
`toolchain/lazurite-albite-compat`（LGPL-3.0，含为 albite 增加 BGFX 预处理的修改），以及示例包使用的
`bgfx_shader.sh`。Mirai 与 newb-x 仅作为兼容性测试对象；当前公开源码仓库不包含它们的适配 ZIP，
相关上游许可证与版权仍以原项目为准。

其他第三方编译器或运行时组件不属于本仓库分发范围，除非在上表中明确列出。

If third-party source code is incorporated in the future, the corresponding copyright notices,
license text, modification notices, and source-code obligations must be preserved for the affected
files.
