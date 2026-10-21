-- LeviLamina 客户端模组（Minecraft Bedrock 26.51 / LeviLamina 26.51.x）
--
-- 配置： xmake f -y -p windows -a x64 -m release --target_type=client
-- 构建： xmake -y                 （产物：bin/RenderDragonShaderLoader/）
-- 调试： xmake f -y -p windows -a x64 -m debug   --target_type=client && xmake -y

add_rules("mode.debug", "mode.release")

if is_plat("windows") then
    add_defines("_CRT_SECURE_NO_WARNINGS")
end

-- LeviMC 的包仓库（levilamina / levibuildscript / bedrockdata 等都在这里）
add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")      -- 本框架默认构建客户端模组
    set_showmenu(true)
    set_values("server", "client")
option_end()

-- 测试用安装目录：构建后自动把 bin/<模组名>/ 同步到 <mods_dir>/<模组名>/。
-- 用 xmake -y --mods_dir=<路径> 覆盖；目录不存在时自动跳过（换机器也能正常构建）。
option("mods_dir")
    set_default("")
    set_showmenu(true)
    set_description("构建后自动同步到的 mods 目录（通过 --mods_dir 传入；留空则只构建）")
option_end()

-- 固定 LeviLamina 版本：26.51.* 会解析到 26.51 系列的最新版（当前 26.51.7）
-- 也可写死具体版本：add_requires("levilamina 26.51.6", ...)
-- 或使用开发版：   add_requires("levilamina develop", ...)（此时需自行添加 bdslibrary）
add_requires("levilamina 26.51.*", {configs = {target_type = get_config("target_type")}})

-- 打包与链接规则：自动生成 manifest.json，并把 dll 输出到 bin/<模组名>/
add_requires("levibuildscript")

-- Header-only JSON used by pack metadata, compile meta.json, and offline tools.
add_requires("nlohmann_json")

-- 本机没有 clang-cl（VS 未装 Clang 工具，也没装 LLVM），让 xmake 自行下载预编译 LLVM。
add_requires("llvm 21.1.0", {alias = "llvm"})

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("RenderDragonShaderLoader") -- 改成你的模组名（只允许英文字母、数字、中划线）
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {modVersion = "0.2.1"})
    if is_plat("windows") then
        -- LeviLamina 26.51 的 SDK 头（ll/api/reflection/Reflection.h）用了 std::to_underlying
        -- 这类 C++23 设施，而 SDK 自己的构建设的是 c++20 + _HAS_CXX23=1（见 SDK 的 xmake.lua
        -- "work around to enable c++23"）。这个 define 不是 public 的，所以使用方要自己加。
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
        -- user32：前台窗口 / F9 诊断键；shell32：WM_DROPFILES 文件拖放；
        -- comdlg32：设置页「导入 / 导出」的系统文件对话框。
        add_syslinks("user32", "shell32", "comdlg32")
        set_exceptions("none") -- 避免与 /EHa 冲突
        add_cxflags("/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        add_shflags("/Brepro", "/PDBALTPATH:RenderDragonShaderLoader.pdb")
        set_toolchains("clang-cl", {packages = "llvm"})
    end
    add_packages("levilamina")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")
    if is_config("target_type", "server") then
        add_includedirs("src-server")
        local serverSources = os.files("src-server/**.cpp")
        if #serverSources > 0 then add_files(serverSources) end
    else
        add_includedirs("src-client")
        local clientSources = os.files("src-client/**.cpp")
        if #clientSources > 0 then add_files(clientSources) end
    end

    -- 构建后处理：
    --   1) 把随模组分发的编译工具链（toolchain/*.exe）放进 bin/<模组名>/toolchain/；
    --   2) 调 tools/sync_mod.ps1 把 bin/<模组名>/ 同步到 <mods_dir>/<模组名>/。
    -- 同步交给 PowerShell 脚本做：xmake 的 Lua 沙箱没有 pcall/try/io，
    -- 而游戏运行时 DLL 被占用会让 os.cp 抛错、把整个构建判失败。
    after_build(function(target)
        local modName = target:name()
        local binDir  = path.join(os.projectdir(), "bin", modName)

        local toolchainSrc = path.join(os.projectdir(), "toolchain")
        if os.isdir(toolchainSrc) then
            local toolchainDst = path.join(binDir, "toolchain")
            os.tryrm(toolchainDst)
            local ok, err = os.cp(toolchainSrc, binDir)
            if not os.isdir(toolchainDst) then
                print(string.format("[sync] warning: toolchain copy failed (%s)", tostring(err)))
            end
            for _, patchName in ipairs({
                "patch_material_uniform.py",
                "patch_material_blobs.py",
                "material_io_dump.py",
                "native_io_validate.py",
                "native_io_map.py"
            }) do
                local patchTool = path.join(os.projectdir(), "tools", patchName)
                if os.isfile(patchTool) then
                    os.mkdir(toolchainDst)
                    os.cp(patchTool, toolchainDst)
                    local patchDst = path.join(toolchainDst, patchName)
                    if not os.isfile(patchDst) then
                        print("[sync] warning: " .. patchName .. " copy failed")
                    end
                end
            end
            local contractSrc = path.join(
                os.projectdir(),
                "samples", "compat", "RenderDragon-26.51.vanilla.material-io.json"
            )
            if os.isfile(contractSrc) then
                local contractDst = path.join(toolchainDst, "compat")
                os.mkdir(contractDst)
                os.cp(contractSrc, contractDst)
                if not os.isfile(path.join(
                    contractDst, "RenderDragon-26.51.vanilla.material-io.json"
                )) then
                    print("[sync] warning: native IO contract copy failed")
                end
                local catalogSrc = path.join(
                    os.projectdir(),
                    "samples", "compat", "native-io-mapping-catalog.json"
                )
                if os.isfile(catalogSrc) then
                    os.cp(catalogSrc, contractDst)
                    if not os.isfile(path.join(
                        contractDst, "native-io-mapping-catalog.json"
                    )) then
                        print("[sync] warning: native mapping catalog copy failed")
                    end
                end
            end
        end

        -- 语言文件：lang/*.json（对照 OptiFine/Iris 的多语言，目前 zh_CN + en_US）
        local langSrc = path.join(os.projectdir(), "lang")
        if os.isdir(langSrc) then
            local langDst = path.join(binDir, "lang")
            os.mkdir(langDst)
            for _, f in ipairs(os.files(path.join(langSrc, "*.json"))) do
                os.cp(f, langDst)
            end
        end

        -- 默认配置：<modDir>/config/config.json —— 让第一次装上的人拿到的就是作者调好的观感
        -- （比如 uiBackdrop）。这里每次都覆盖 bin/ 里的那份；**安装目录**里的那份由
        -- tools/sync_mod.ps1 保护，玩家改过的设置不会被构建覆盖。
        local configSrc = path.join(os.projectdir(), "config", "config.json")
        if os.isfile(configSrc) then
            local configDst = path.join(binDir, "config")
            os.mkdir(configDst)
            os.cp(configSrc, path.join(configDst, "config.json"))
        end

        local modsDir = get_config("mods_dir")
        if not (modsDir and os.isdir(modsDir)) then
            return
        end

        local script = path.join(os.projectdir(), "tools", "sync_mod.ps1")
        if not os.isfile(script) then
            print("[sync] warning: tools/sync_mod.ps1 not found; skip install")
            return
        end

        os.execv("powershell", {
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", script,
            "-Source", binDir,
            "-Dest", path.join(modsDir, modName)
        })
    end)

-- ---------------------------------------------------------------------------
-- 独立 UI 预览器：把 src/ui 的界面逻辑单独编译成一个 Win32 窗口程序，
-- 不依赖 LeviLamina / 不启动游戏，用来快速调界面与交互。
--   构建： xmake build ui_preview            产物：build/<平台>/<arch>/release/ui_preview.exe
--   运行： xmake run ui_preview [shaderpacks 目录] [--scale N] [--no-compile]
-- ---------------------------------------------------------------------------
target("ui_preview")
    set_kind("binary")
    set_languages("c++20")
    add_defines("NOMINMAX")
    -- 让预览器知道源码树位置，用来找 lang/*.json 与 toolchain/shaderc.exe
    -- （统一转成正斜杠，否则宏里的反斜杠会被当成转义序列）
    local uiPreviewSourceDir = (os.projectdir()):gsub("\\", "/")
    add_defines('UI_PREVIEW_SOURCE_DIR="' .. uiPreviewSourceDir .. '"')
    if is_plat("windows") then
        add_cxflags("/utf-8", "/W3", {tools = {"clang_cl"}})
        set_exceptions("cxx")
        set_toolchains("clang-cl", {packages = "llvm"})
        add_syslinks("user32", "gdi32", "comdlg32")
    else
        set_exceptions("cxx")
    end
    add_files("tools/ui_preview/*.cpp")
    add_files(
        "src/ui/UiScreen.cpp",
        "src/ui/UiLayout.cpp",
        "src/ui/UiSettings.cpp",
        "src/ui/ConfigurationName.cpp"
    )
    add_files(
        "src/core/ShaderPack.cpp",
        "src/core/CompilerService.cpp",
        "src/core/SourcePatcher.cpp",
        "src/core/PackMetadata.cpp",
        "src/core/Hashing.cpp",
        "src/core/MacroDependency.cpp",
        "src/core/CompileCache.cpp",
        "src/core/MaterialValidation.cpp"
    )
    add_files("src/core/Hotkey.cpp")
    add_packages("nlohmann_json")
    add_includedirs("src")

-- Offline smoke test: runs the real CompilerService against a project and
-- verifies the published material contract.
target("compile_smoke")
    set_kind("binary")
    set_languages("c++20")
    add_files("tools/compile_smoke.cpp")
    add_files(
        "src/core/CompilerService.cpp",
        "src/core/PackMetadata.cpp",
        "src/core/SourcePatcher.cpp",
        "src/core/Hashing.cpp",
        "src/core/MacroDependency.cpp",
        "src/core/CompileCache.cpp",
        "src/core/MaterialValidation.cpp"
    )
    if is_plat("windows") then
        add_cxflags("/utf-8", "/W3", {tools = {"clang_cl"}})
        set_exceptions("cxx")
        set_toolchains("clang-cl", {packages = "llvm"})
    else
        set_exceptions("cxx")
    end
    add_packages("nlohmann_json")
    add_includedirs("src")

-- Offline package scanner used by docs and preset debugging.
target("pack_scan_dump")
    set_kind("binary")
    set_languages("c++20")
    add_files("tools/pack_scan_dump.cpp")
    add_files(
        "src/core/ShaderPack.cpp",
        "src/core/PackMetadata.cpp",
        "src/core/MaterialValidation.cpp",
        "src/core/Hashing.cpp"
    )
    if is_plat("windows") then
        add_cxflags("/utf-8", "/W3", {tools = {"clang_cl"}})
        set_exceptions("cxx")
        set_toolchains("clang-cl", {packages = "llvm"})
    else
        set_exceptions("cxx")
    end
    add_packages("nlohmann_json")
    add_includedirs("src")

-- Regression test for the preset selected immediately before compilation:
-- a later rescan must not restore an older empty preset.
target("preset_state_test")
    set_kind("binary")
    set_languages("c++20")
    add_files("tools/preset_state_test.cpp")
    add_files(
        "src/core/ShaderPack.cpp",
        "src/core/PackMetadata.cpp",
        "src/core/MaterialValidation.cpp",
        "src/core/Hashing.cpp"
    )
    if is_plat("windows") then
        add_cxflags("/utf-8", "/W3", {tools = {"clang_cl"}})
        set_exceptions("cxx")
        set_toolchains("clang-cl", {packages = "llvm"})
    else
        set_exceptions("cxx")
    end
    add_packages("nlohmann_json")
    add_includedirs("src")

target("configuration_name_test")
    set_kind("binary")
    set_languages("c++20")
    add_files("tools/configuration_name_test.cpp", "src/ui/ConfigurationName.cpp")
    if is_plat("windows") then
        add_cxflags("/utf-8", "/W3", {tools = {"clang_cl"}})
        set_exceptions("cxx")
        set_toolchains("clang-cl", {packages = "llvm"})
    else
        set_exceptions("cxx")
    end
    add_includedirs("src")
