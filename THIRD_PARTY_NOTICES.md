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

## Third-Party Binaries / 第三方二进制

This repository does not distribute third-party compiler binaries or shader compiler services such
as shaderc, lazurite, Mirai, or their bundled components.

本仓库不分发 shaderc、lazurite、Mirai 等第三方编译器或编译器组件。

If third-party source code is incorporated in the future, the corresponding copyright notices,
license text, modification notices, and source-code obligations must be preserved for the affected
files.
