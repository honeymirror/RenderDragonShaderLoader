# RenderDragonShaderLoader

English | [简体中文](README.md)

A **LeviLamina client mod** for Minecraft Bedrock (RenderDragon) that **manages third-party
shader packs**, supports **hot reload**, and provides an Iris-style interface for adjusting
custom shader parameters.

> Target versions: **Minecraft Bedrock 1.26.51** (client) + **LeviLamina 26.51.x** (Windows x64)
>
> Runtime dependency: **Python 3.10 or newer** (Python 3.12+ recommended)
>
> Current release: **0.2.0-beta** (pre-release; includes compatibility reports, Native IO mappings,
> reproducible builds, and configuration management fixes)
>
> Import format: `shaderpacks/` is read as `.zip`. Dropping a `.mcpack` onto the game window is
> also supported: it is **renamed to `.zip` automatically** before importing (a `.mcpack` is a zip
> container).

## Python Runtime

The current release uses bundled Python scripts for shader compilation and Native IO compatibility
validation. Install **Python 3.10+** before running the mod:

1. Install Python 3 from [python.org](https://www.python.org/downloads/windows/).
2. Enable **Python Launcher** and **Add Python to PATH** during installation.
3. Fully exit the game, then run this command in PowerShell or Command Prompt:

   ```powershell
   py -3 --version
   ```

4. Start the game after the Python version is displayed successfully.

If the system only provides `python.exe` and does not have `py.exe`, edit
`config/config.json` in the mod directory and replace both commands with the absolute path to
`python.exe`:

```json
"lazuriteCommand": "\"C:\\Python-Path\\python.exe\" toolchain/lazurite_albite.py",
"lazuriteAlbiteCommand": "\"C:\\Python-Path\\python.exe\" toolchain/lazurite_albite.py",
```

Replace the example path with the actual local `python.exe` path and keep the escaped quotes.
Fully restart the game after changing the file.

## Current Features

### Panel and Controls

- Press `G` by default to open or close the shader management panel.
- While typing in chat, a sign, an anvil, a book or a command block, the panel hotkey **steps
  aside**: the key goes into the text box instead of opening the panel (and is not swallowed).
- Browse the shader list with the mouse and drag the scrollbar. You can also drag a `.zip` shader
  or `.mcpack` shader pack directly into the Minecraft window to import it.
- Shader changes are applied only after clicking Apply / Done. Clicking Cancel or pressing `Esc`
  discards the unapplied changes.
- The Settings button at the bottom left opens the mod settings page: **rebind the panel hotkey**
  (press a key to record it), force-enable Vibrant Visuals, auto-load the last used shader pack on
  startup, and override the graphics mode manually.
- The arrow keys move between the content area, the two rows of bottom buttons, the top bar
  buttons (Import / Export / Reset) and the bottom-right transparency button; Enter acts as a left
  click.

### Shader Pack Recognition and Import

- The shader pack directory is `data/shaderpacks/` under the mod directory.
- When importing through drag and drop, `.zip` and `.mcpack` files are accepted. A dragged
  `.mcpack` is stored as `<name>.zip`; a `.mcpack` placed in `data/shaderpacks/` manually is
  renamed the same way when the directory is scanned.
- `.zip` files are automatically extracted to `data/generated/<pack-name>/src/` as working copies.
- An already extracted shader directory can also be placed manually in `data/shaderpacks/`, but it
  must contain `project.json` or `shaders.properties` to be recognized.
- Packs can load prebuilt `.material.bin` files directly or include source for the mod to compile.
  A source pack without output enters the compilation pipeline automatically.

### Shader List and Parameter Interface

- The list shows full pack names. The mouse wheel only scrolls the list and does not change the
  current selection.
- The graphics mode is switched automatically based on the output type: deferred output uses
  Vibrant Visuals, and forward output uses Fancy.
  (The detection looks for `Deferred*` in the output file names, so it is a heuristic; the
  "Graphics mode override" entry in the settings page lets the player correct it.)
- The shader pack that was enabled when the panel was closed / the game exited is remembered and
  loaded again on the next startup (can be turned off in the settings page).
- The custom parameter interface follows Iris conventions: it has category and parameter pages,
  with both two-column and full-width layouts.
- Supported parameter controls include sliders, checkboxes, enums, and colors. Sliders expand on
  hover, and their handles can be dragged.
- Parameters come from the pack's own `options.json` or `shaders.properties`. If no supported
  declarations are present, the interface shows an empty state.
- See [`CUSTOM-PARAMETERS.md`](CUSTOM-PARAMETERS.md) for the complete parameter schema, source
  integration rules, groups, localization, and runtime uniform setup. The guide is in Simplified
  Chinese.
- The top bar provides Import, Export, and Reset. Status messages fade out after 3 seconds, and the
  interface uses Iris-style click sounds.

### `/shader` Commands

| Command | Description |
| --- | --- |
| `/shader scan` | Rescan `data/shaderpacks/` and refresh the shader pack list. |
| `/shader ui` | Open or close the shader management panel directly, without using the hotkey. |
| `/shader key` | Show the current hotkey used to open the panel. |
| `/shader key <key>` | Change the panel hotkey. Supports `A-Z`, `0-9`, `F1-F12`, `SPACE`, `TAB`, `UP`, and similar names. Use `NONE` to disable the hotkey. |
| `/shader vibrant` | Show the state of "force enable Vibrant Visuals". |
| `/shader vibrant <on\|off>` | Toggle force-enabling Vibrant Visuals (some servers lock the graphics mode; this bypasses the check). |
| `/shader list` | List recognized shader packs in chat, including the number of parsed parameters for each pack. |
| `/shader rollback` | Disable material redirection and restore vanilla materials. This succeeds only when hot reload support is available. |

## Compilation And Outputs

- Compilation is enabled by default: `lazurite build` runs with the bundled
  `toolchain/shaderc.exe` through a serialized job queue with cancellation, timeout, and logs.
- Parameter edits trigger material-level incremental compilation. Cache hits start no compiler
  process at all.
- Outputs are written to `compiled.new/`, validated for magic, target material format version 26, count,
  and `meta.json`, then atomically switched into `compiled/`. Old output stays under
  `compiled/.backup/`.
- Packs may ship prebuilt `.material.bin` files, source for compilation, or both. Successful
  compilation is followed by material hot reload.
- `rdloader.pack.json`, `options.json`, `variants.json`, and pack-local `lang/*.lang` are supported
  while keeping plain `.zip`, `.mcpack`, and `shaders.properties` compatibility.

## Build

Requirements: Windows x64 + Python 3.10+ + xmake + Visual Studio (MSVC) + LLVM (clang-cl) + the
LeviLamina 26.51.x SDK.

```bash
xmake f -y -p windows -a x64 -m release --target_type=client --mods_dir=<game mods directory>
xmake -y
```

The built mod package is `bin/RenderDragonShaderLoader/`. When `--mods_dir` is configured, the
build also syncs it to `mods/RenderDragonShaderLoader/`; snapshots are kept in `_backup/` and can
be managed with `tools/sync_mod.ps1 -List` / `-Rollback`.

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
```

## Third-Party References

### Design and Compatibility References

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

### Build Toolchain and Bundled Components

- [Lazurite](https://github.com/veka0/lazurite): LGPL-3.0. Used as the shader-project build entry.
- [lazurite-albite-compat](https://github.com/bambosan/lazurite-albite-compat): LGPL-3.0. The
  albite-compatible Lazurite source tree under `toolchain/lazurite-albite-compat/`.
- [Albite](https://github.com/bambosan/albite): MIT License. Optional material compiler backend.

- License: this project is licensed under LGPL-3.0. See [`LICENSE`](LICENSE).
- Third-party projects, dependencies, and distribution notes are listed in
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
