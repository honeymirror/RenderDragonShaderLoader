-- LeviLamina 客户端模组（Minecraft Bedrock 26.40 / LeviLamina 26.40.x）
--
-- 配置： xmake f -y -p windows -a x64 -m release --target_type=client
-- 构建： xmake -y                 （产物：bin/RenderDragonShaderLoader/）
-- 调试： xmake f -y -p windows -a x64 -m debug   --target_type=client && xmake -y

add_rules("mode.debug", "mode.release")

-- LeviMC 的包仓库（levilamina / levibuildscript / bedrockdata 等都在这里）
add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")      -- 本框架默认构建客户端模组
    set_showmenu(true)
    set_values("server", "client")
option_end()

-- 固定 LeviLamina 版本：26.40.* 会解析到 26.40 系列的最新版（当前 26.40.6）
-- 也可写死具体版本：add_requires("levilamina 26.40.6", ...)
-- 或使用开发版：   add_requires("levilamina develop", ...)（此时需自行添加 bdslibrary）
add_requires("levilamina 26.40.*", {configs = {target_type = get_config("target_type")}})

-- 打包与链接规则：自动生成 manifest.json，并把 dll 输出到 bin/<模组名>/
add_requires("levibuildscript")

-- 本机没有 clang-cl（VS 未装 Clang 工具，也没装 LLVM），让 xmake 自行下载预编译 LLVM。
add_requires("llvm 21.1.0", {alias = "llvm"})

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("RenderDragonShaderLoader") -- 改成你的模组名（只允许英文字母、数字、中划线）
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {modVersion = "0.1.0-beta"})
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
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
        set_toolchains("clang-cl", {packages = "llvm"})
    end
    add_packages("levilamina")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")

    -- 构建后处理：把语言文件（lang/*.json）和默认配置（config/config.json）一起放进
    -- bin/<模组名>/，凑成一个可以直接安装的模组包。
    -- 装到游戏目录是使用者自己的事（把 bin/<模组名>/ 拷进 mods/ 即可），构建过程不碰任何游戏目录。
    after_build(function(target)
        local modName = target:name()
        local binDir  = path.join(os.projectdir(), "bin", modName)

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
        -- （比如 uiBackdrop）。这里每次都覆盖 bin/ 里的那份；已经装到 mods/ 里的那份属于玩家，
        -- 发布/升级时不要覆盖它（模组只在配置文件不存在时才写默认值）。
        local configSrc = path.join(os.projectdir(), "config", "config.json")
        if os.isfile(configSrc) then
            local configDst = path.join(binDir, "config")
            os.mkdir(configDst)
            os.cp(configSrc, path.join(configDst, "config.json"))
        end

    end)
