# RenderDragonShaderLoader

English | [简体中文](README.md)

A **LeviLamina client mod** for Minecraft Bedrock (RenderDragon) that **manages third-party
shader packs**, supports **hot reload**, and provides an Iris-style interface for adjusting
custom shader parameters.

> Target versions: **Minecraft Bedrock 1.26.40** (client) + **LeviLamina 26.40.x** (Windows x64)
>
> Current release: **0.1.0-beta** (pre-release; includes shader pack management and the parameter
> interface only, see "Not Implemented Yet" below)
>
> Import format: currently only `.zip` shader packs are supported. `.mcpack` is **not supported
> yet**. To use a `.mcpack`, rename its extension to `.zip` before importing it.

## Current Features

### Panel and Controls

- Press `G` by default to open or close the shader management panel.
- Browse the shader list with the mouse and drag the scrollbar. You can also drag a `.zip` shader
  pack directly into the Minecraft window to import it.
- Shader changes are applied only after clicking Apply / Done. Clicking Cancel or pressing `Esc`
  discards the unapplied changes.

### Shader Pack Recognition and Import

- The shader pack directory is `data/shaderpacks/` under the mod directory.
- When importing through drag and drop or the import function, only `.zip` files are accepted.
  `.mcpack` files are skipped and do not appear in the shader list.
- `.zip` files are automatically extracted to `data/generated/<pack-name>/src/` as working copies.
- An already extracted shader directory can also be placed manually in `data/shaderpacks/`, but it
  must contain `project.json` or `shaders.properties` to be recognized.
- This version only loads prebuilt `.material.bin` files included in the shader pack. A pack can be
  used only if it contains compiled output.

### Shader List and Parameter Interface

- The list shows full pack names. The mouse wheel only scrolls the list and does not change the
  current selection.
- The graphics mode is switched automatically based on the output type: deferred output uses
  Vibrant Visuals, and forward output uses Fancy.
- The custom parameter interface follows Iris conventions: it has category and parameter pages,
  with both two-column and full-width layouts.
- Supported parameter controls include sliders, checkboxes, enums, and colors. Sliders expand on
  hover, and their handles can be dragged.
- Parameters come from the real declarations in the shader pack's `shaders.properties`. If a pack
  declares no parameters, the interface shows an empty state.
- The top bar provides Import, Export, and Reset. Status messages fade out after 3 seconds, and the
  interface uses Iris-style click sounds.

### `/shader` Commands

| Command | Description |
| --- | --- |
| `/shader scan` | Rescan `data/shaderpacks/` and refresh the shader pack list. |
| `/shader ui` | Open or close the shader management panel directly, without using the hotkey. |
| `/shader key` | Show the current hotkey used to open the panel. |
| `/shader key <key>` | Change the panel hotkey. Supports `A-Z`, `0-9`, `F1-F12`, `SPACE`, `TAB`, `UP`, and similar names. Use `NONE` to disable the hotkey. |
| `/shader list` | List recognized shader packs in chat, including the number of parsed parameters for each pack. |
| `/shader rollback` | Disable material redirection and restore vanilla materials. This succeeds only when hot reload support is available. |

## Not Implemented Yet

- **No compiler pipeline yet**: the mod does not currently invoke lazurite or shaderc to compile
  shaders. It **only loads prebuilt `.material.bin` files included in the shader pack**, so whether
  a pack works depends on whether it already contains compiled output (see
  [`docs/shader-pack-format.md`](docs/shader-pack-format.md)). Custom parameter compilation is
  still under development.
- The settings page currently only stores parameter values in
  `data/shaderpacks/<pack-name>.zip.txt`. For those values to affect rendering, the compiler
  pipeline must be connected, unless the pack already includes matching output.
- The hot reload implementation (`readFile` redirection + `reloadMaterial`) is present, but it has
  not been fully validated in game yet.

## Build

Requirements: Windows x64 + xmake + Visual Studio (MSVC) + LLVM (clang-cl) + the LeviLamina 26.40.x
SDK.

```bash
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -y
```

The output is a directly installable mod package. Copy the entire `bin/RenderDragonShaderLoader/`
directory into the game's `mods/` directory. `xmake.lua` includes `lang/` and `config/config.json`
in the output, and the build process does not modify any game directory.

## Directory Structure

```
src/            Mod source code, independent from the game runtime and buildable/testable offline
  mod/          Mod class, configuration, and `/shader` command registration
  core/         Shader pack scanning/parsing, parameter models, and graphics mode switching
  hook/         Material hot reload (readFile redirection + reloadMaterial), window drag-and-drop import
  ui/           UI (UiScreen / UiSettings / UiLayout / UiCanvas) and game adapter layer
lang/           Localization files (zh_CN / en_US)
config/         Default configuration for first installation
samples/        Example shader pack (ExamplePack)
docs/           Shader pack format documentation
```

## Third-Party References

- [Iris](https://github.com/IrisShaders/Iris): reference for the shader parameter UI, parameter
  categories, slider interaction, and click sound behavior.
- [OptiFine](https://optifine.net/): reference for `shaders.properties` parameter expressions and
  the `shaderpacks` directory convention.
- [BetterRenderDragon](https://github.com/QYCottage/BetterRenderDragon): reference for the material
  hot reload approach.
- [Lamium](https://github.com/amatouhake/Lamium): **this mod's UI is based on its implementation**,
  including the custom-drawn screen and input handling.

No third-party binaries or resources from these projects are distributed with this repository.
They are listed only as design and implementation references.

- License: see `LICENSE`.
