#include "core/ShaderPack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace renderdragon_shader_loader {
namespace {

std::string_view trimView(std::string_view s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string trim(std::string_view s) { return std::string(trimView(s)); }

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::vector<std::string> splitBy(std::string_view s, char delim) {
    std::vector<std::string> out;
    std::size_t              start = 0;
    while (true) {
        std::size_t pos = s.find(delim, start);
        out.emplace_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return out;
}

// 按空白和逗号切分（shaders.properties 两种分隔符都常见）。
std::vector<std::string> splitTokens(std::string_view s) {
    std::vector<std::string> out;
    std::string              current;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == ',' || c == '\r' || c == '\n') {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

bool isInteger(std::string_view s) {
    if (s.empty()) return false;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}

bool parseDouble(std::string_view s, double& out) {
    std::string tmp(trimView(s));
    if (tmp.empty()) return false;
    char* end = nullptr;
    out       = std::strtod(tmp.c_str(), &end);
    return end != tmp.c_str() && *end == '\0';
}

bool isNumber(std::string_view s) {
    double dummy;
    return parseDouble(s, dummy);
}

bool isBoolWord(std::string const& s) {
    std::string v = toLower(s);
    return v == "on" || v == "off" || v == "true" || v == "false" || v == "yes" || v == "no"
        || v == "1" || v == "0";
}

bool hasMaterialBin(std::filesystem::path const& dir) {
    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().filename().string().ends_with(".material.bin")) return true;
    }
    return false;
}

#ifdef _WIN32
// 静默跑一条命令，返回退出码（-1 = 启动失败）。
int runHidden(std::wstring cmdline) {
    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            nullptr,
            cmdline.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        )) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#endif

// 把 zip 解到 dest。Windows 10+ 自带 tar.exe，可直接解 zip。
bool extractZip(std::filesystem::path const& zip, std::filesystem::path const& dest) {
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    std::wstring cmd = L"tar.exe -xf \"" + zip.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
    return runHidden(std::move(cmd)) == 0;
#else
    (void)zip;
    (void)dest;
    return false;
#endif
}

// zip 只在首次或内容变化时解压；用一个 marker 记录 zip 的修改时间。
bool ensureExtracted(std::filesystem::path const& zip, std::filesystem::path const& dest) {
    std::error_code ec;
    auto            zipTime = std::filesystem::last_write_time(zip, ec);
    if (ec) return false;
    auto mtime = (long long)zipTime.time_since_epoch().count();

    auto marker = dest / ".source.zip";
    if (std::filesystem::is_regular_file(marker, ec)) {
        std::ifstream in(marker);
        long long     stored = 0;
        if ((in >> stored) && stored == mtime) return true; // 已是最新
    }

    std::filesystem::remove_all(dest, ec);
    std::filesystem::create_directories(dest, ec);
    if (!extractZip(zip, dest)) return false;

    std::filesystem::create_directories(dest, ec);
    std::ofstream out(marker);
    out << mtime;
    return true;
}

bool hasPackMarker(std::filesystem::path const& dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "project.json", ec)
        || std::filesystem::is_regular_file(dir / "shaders.properties", ec)
        // 只带编译产物、没有 shader 源码的"发行版"包（load-only 模式也能用）：目录里有
        // *.material.bin 就认。没有这条，玩家直接把一堆 .bin 丢进 shaderpacks 会被跳过。
        || hasMaterialBin(dir);
}

// 路径里如果有 `subpacks/<名字>/` 这一段，返回那个 <名字>（否则空串）。
std::string subpackNameOf(std::filesystem::path const& p) {
    auto        s   = p.generic_string(); // 统一用 /
    auto const  key = std::string("/subpacks/");
    auto        pos = s.find(key);
    if (pos == std::string::npos) return {};
    auto rest  = s.substr(pos + key.size());
    auto slash = rest.find('/');
    return slash == std::string::npos ? rest : rest.substr(0, slash);
}

// 收集一个包里所有"直接含 *.material.bin"的目录，并按资源包 subpack 的覆盖规则排序：
//   1) 不含 subpacks/ 的基础目录在前；
//   2) 含 subpacks/<名>/ 的只保留**名字最大的那个变体**（Mirai: novc < vc ⇒ 选 vc）；
//   3) 同级按路径排序，结果稳定。
//
// 递归是为了兼容各种外壳目录：Mirai 的 zip 里产物其实在
// `Mirai/Mirai/render/renderer/materials`（多一层 render/），硬编码 renderer/materials 会漏。
// 跳过 vanilla/（合并源）和 . / _ 开头的内部目录。
std::vector<std::filesystem::path> collectPrebuiltDirs(std::filesystem::path const& root) {
    std::vector<std::filesystem::path> base;
    std::map<std::string, std::vector<std::filesystem::path>> subpacks;

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return {};

    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        ec
    );
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;

        auto name = it->path().filename().string();
        if ((!name.empty() && (name[0] == '.' || name[0] == '_')) || name == "vanilla") {
            it.disable_recursion_pending(); // 合并源 / 内部目录不算产物
            continue;
        }
        if (!hasMaterialBin(it->path())) continue;

        auto sub = subpackNameOf(it->path());
        if (sub.empty()) base.push_back(it->path());
        else subpacks[sub].push_back(it->path());
        it.disable_recursion_pending(); // 这一层已经是产物目录，别再往里挖
    }

    std::sort(base.begin(), base.end());
    std::vector<std::filesystem::path> out = base;

    if (!subpacks.empty()) {
        auto const& [name, dirs] = *subpacks.rbegin(); // map 有序 ⇒ 名字最大的变体
        out.insert(out.end(), dirs.begin(), dirs.end());
    }
    return out;
}

// zip 里常有一层外壳目录（如 mirai-continued-main/），或者包自己的项目在 src/ 下。
// 找到真正能交给 lazurite 的项目根。
std::filesystem::path normalizeSourceDir(std::filesystem::path const& dir) {
    std::error_code ec;
    if (hasPackMarker(dir)) return dir;

    // 包自己的项目在 src/（mirai 这类“仓库式”布局）
    if (hasPackMarker(dir / "src")) return dir / "src";

    // 只有一层外壳目录就钻进去
    std::vector<std::filesystem::path> subdirs;
    for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_directory(ec)) subdirs.push_back(e.path());
    }
    if (subdirs.size() == 1) {
        auto inner = normalizeSourceDir(subdirs[0]);
        if (hasPackMarker(inner) || std::filesystem::is_directory(inner / "renderer", ec)) return inner;
    }
    return dir;
}

// 从 `option.NAME = <tokens>` 推断参数类型。
//   空                 -> Bool（默认关）
//   ON/OFF、true/false -> Bool（默认取第一个）
//   2 个数字           -> Slider(min max)，默认 min
//   3 个数字           -> Slider(min max default)
//   其它多值           -> Enum（默认取第一个）
void fillParameter(ShaderParameter& p, std::vector<std::string> const& tokens) {
    p.type = ParameterType::Bool;

    if (tokens.empty()) {
        p.values  = {"OFF", "ON"};
        p.current = "OFF";
        return;
    }

    bool allBool = std::all_of(tokens.begin(), tokens.end(), [](std::string const& t) { return isBoolWord(t); });
    bool allNum  = std::all_of(tokens.begin(), tokens.end(), [](std::string const& t) { return isNumber(t); });

    if (allBool) {
        p.type    = ParameterType::Bool;
        p.values  = tokens;
        p.current = tokens.front();
        return;
    }

    if (allNum && (tokens.size() == 2 || tokens.size() == 3)) {
        p.type = ParameterType::Slider;
        parseDouble(tokens[0], p.min);
        parseDouble(tokens[1], p.max);
        p.def     = tokens.size() == 3 ? (parseDouble(tokens[2], p.def), p.def) : p.min;
        p.integer = isInteger(tokens[0]) && isInteger(tokens[1]);
        p.current = tokens.size() == 3 ? tokens[2] : tokens[0];
        return;
    }

    p.type    = ParameterType::Enum;
    p.values  = tokens;
    p.current = tokens.front();
}

} // namespace

ShaderPackStore& ShaderPackStore::getInstance() {
    static ShaderPackStore instance;
    return instance;
}

ShaderPack const* ShaderPackStore::find(std::string_view name) const {
    for (auto const& pack : mPacks) {
        if (pack.name == name) return &pack;
    }
    return nullptr;
}

bool ShaderPackStore::setParameterValue(std::string_view packName, std::size_t index, std::string value) {
    for (auto& pack : mPacks) {
        if (pack.name != packName) continue;
        if (index >= pack.parameters.size()) return false;
        pack.parameters[index].current = std::move(value);
        return true;
    }
    return false;
}

void ShaderPackStore::scan(std::filesystem::path const& packsRoot, std::filesystem::path const& generatedRoot) {
    mPacks.clear();
    std::error_code ec;

    if (!std::filesystem::is_directory(packsRoot, ec)) {
        std::filesystem::create_directories(packsRoot, ec);
        return;
    }

    for (auto const& entry : std::filesystem::directory_iterator(packsRoot, ec)) {
        auto fileName = entry.path().filename().string();
        if (!fileName.empty() && (fileName[0] == '.' || fileName[0] == '_')) continue;

        if (entry.is_directory(ec)) {
            if (toLower(fileName) == "vanilla") continue;
            if (!hasPackMarker(entry.path())) continue;

            // 目录形式的包：直接用原目录做源，产物在 generated/<名字>/compiled/。
            auto bins = generatedRoot / fileName / "compiled";
            mPacks.push_back(
                loadPack(entry.path(), {}, bins, packsRoot / (fileName + ".txt"))
            );
            continue;
        }

        if (!entry.is_regular_file(ec)) continue;
        if (toLower(entry.path().extension().string()) != ".zip") continue;

        // .zip 形式的包：解压到 generated/<名字>/src/ 作为工作副本。
        auto name = entry.path().stem().string();
        auto src  = generatedRoot / name / "src";
        if (!ensureExtracted(entry.path(), src)) {
            // 解压失败（空 zip / 坏 zip）也照样列出来：Iris 也是这样——玩家能看到自己放进去的
            // 包，真要编译/使用时再报错。以前这里直接 continue，导致刚放进去的空 zip 根本
            // 不出现在列表里（实测就是这个问题）。
            mPacks.push_back(
                loadPack(src, entry.path(), generatedRoot / name / "compiled", packsRoot / (fileName + ".txt"), src)
            );
            continue;
        }

        auto bins = generatedRoot / name / "compiled";
        mPacks.push_back(
            loadPack(normalizeSourceDir(src), entry.path(), bins, packsRoot / (fileName + ".txt"), src)
        );
    }
}

ShaderRenderMode detectRenderMode(std::filesystem::path const& binsDir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(binsDir, ec)) return ShaderRenderMode::Forward;

    for (auto const& entry : std::filesystem::directory_iterator(binsDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto name = toLower(entry.path().filename().string());
        if (!name.ends_with(".material.bin")) continue;
        if (name.find("deferred") != std::string::npos) return ShaderRenderMode::Deferred;
    }
    return ShaderRenderMode::Forward;
}

ShaderPack ShaderPackStore::loadPack(
    std::filesystem::path const& sourceDir,
    std::filesystem::path const& packFile,
    std::filesystem::path const& binsDir,
    std::filesystem::path const& settingsPath,
    std::filesystem::path const& containerDir
) {
    ShaderPack pack;
    pack.name         = sourceDir.filename().string();
    pack.sourceDir    = sourceDir;
    pack.archivePath  = packFile;
    pack.binsDir      = binsDir;
    pack.settingsPath = settingsPath;

    // 名字优先用 zip / 目录名，与 settingsPath、binsDir 保持一致。
    if (!packFile.empty()) {
        pack.name = packFile.stem().string();
    } else {
        pack.name = sourceDir.filename().string();
    }

    pack.profiles   = parseProfiles(sourceDir / "shaders.properties");
    pack.parameters = parseParameters(sourceDir / "shaders.properties");

    // 玩家上次调好的选项（.txt）覆盖默认值。
    applySettings(pack);

    // 包里已经有编译好的 *.material.bin（发行版资源包）：直接拷到 binsDir，无需编译。
    //
    // 发行版一般是资源包布局：renderer/materials/ 是基础材质，subpacks/<名>/renderer/materials/
    // 是变体（Mirai 的 vc = 体积云 / novc = 无体积云，两者有 20 多个同名文件内容不同）。
    // 所以这里按 **基础 → 各 subpack（按名字排序，后面的覆盖前面的）** 依次拷，同名以后者为准；
    // `shaders.properties` 里的 `prebuilt = <相对路径>` 优先级最高，第一个拷。
    // 现成产物的搜索根：包自己的工程目录，加上（zip 包的）解压根。
    // 仓库式包（mirai-continued-main）的发行产物在 `pack/`，是工程目录 `src/` 的兄弟目录，
    // 只从 sourceDir 找会全都漏掉 —— 实测就是"包里明明有 .material.bin，模组却说没有产物"。
    std::vector<std::filesystem::path> prebuiltRoots;
    prebuiltRoots.push_back(sourceDir);
    if (!containerDir.empty() && containerDir != sourceDir) {
        std::error_code e;
        if (std::filesystem::is_directory(containerDir, e)) prebuiltRoots.push_back(containerDir);
    }

    for (auto const& root : prebuiltRoots) {
        pack.prebuiltDir = findPrebuiltMaterials(root);
        if (!pack.prebuiltDir.empty()) break;
    }
    if (!hasMaterialBin(pack.binsDir)) {
        std::error_code                    ec;
        std::vector<std::filesystem::path> dirs;
        auto                               addDir = [&](std::filesystem::path const& d) {
            std::error_code e;
            if (!std::filesystem::is_directory(d, e) || !hasMaterialBin(d)) return;
            if (std::find(dirs.begin(), dirs.end(), d) != dirs.end()) return;
            dirs.push_back(d);
        };

        addDir(pack.prebuiltDir); // shaders.properties 指定 / 探测到的主目录，优先
        for (auto const& root : prebuiltRoots) {
            for (auto const& d : collectPrebuiltDirs(root)) addDir(d);
        }

        if (!dirs.empty()) {
            std::filesystem::create_directories(pack.binsDir, ec);
            for (auto const& dir : dirs) {
                for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
                    if (!e.is_regular_file(ec)) continue;
                    auto name = e.path().filename().string();
                    if (!name.ends_with(".material.bin")) continue;
                    std::error_code stepEc;
                    std::filesystem::copy_file(
                        e.path(),
                        pack.binsDir / name,
                        std::filesystem::copy_options::overwrite_existing,
                        stepEc
                    );
                    (void)stepEc;
                }
            }
        }
    }

    pack.renderMode = detectRenderMode(pack.binsDir);
    return pack;
}

// 在包里找现成的 *.material.bin。
// 顺序：shaders.properties 的 `prebuilt = <相对路径>` -> 常见位置 -> 浅层搜索。
// 搜索会跳过 vanilla/ 和隐藏目录，避免把合并源（vanilla 材质）误当成编译产物。
std::filesystem::path ShaderPackStore::findPrebuiltMaterials(std::filesystem::path const& packDir) {
    std::error_code ec;

    if (std::ifstream in(packDir / "shaders.properties"); in) {
        std::string line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;
            std::size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            if (trim(t.substr(0, eq)) != "prebuilt") continue;

            auto rel = trim(t.substr(eq + 1));
            if (rel.empty()) break;
            auto candidate = packDir / rel;
            if (std::filesystem::is_directory(candidate, ec)) return candidate;
            break;
        }
    }

    for (char const* rel : {"renderer/materials", "pack/renderer/materials", "renderer", "dist", "output"}) {
        auto candidate = packDir / rel;
        if (std::filesystem::is_directory(candidate, ec) && hasMaterialBin(candidate)) return candidate;
    }

    // 浅层搜索兜底（含 pack/subpacks/<名>/renderer/materials 这种子包布局）。
    std::filesystem::recursive_directory_iterator it(
        packDir,
        std::filesystem::directory_options::skip_permission_denied,
        ec
    );
    std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;

        auto name = it->path().filename().string();
        if ((!name.empty() && (name[0] == '.' || name[0] == '_')) || name == "vanilla") {
            it.disable_recursion_pending(); // 合并源 / 内部目录不算产物
            continue;
        }
        if (hasMaterialBin(it->path())) return it->path();
        if (it.depth() >= 5) it.disable_recursion_pending();
    }
    return {};
}

// 解析 OptiFine / Iris 风格的 shaders.properties（子集）：
//
//   option.NAME = <values...>     声明一个参数；类型由取值推断（见 fillParameter）
//   sliders = NAME1 NAME2         旧式写法：把列出的名字强制当作 slider
//   screen.S.G = NAME1 NAME2      界面分组与展示顺序
//   profile.NAME = A=1 B=0        预设（解析保留，暂不参与编译宏）
//   其它键（shadow.* / program.* 等）一律忽略
std::vector<ShaderParameter> ShaderPackStore::parseParameters(std::filesystem::path const& propertiesFile) {
    std::vector<ShaderParameter> params;
    std::ifstream                in(propertiesFile);
    if (!in) return params;

    std::vector<std::string>           declared;
    std::map<std::string, std::string> values;
    std::vector<std::string>           screenOrder;
    std::map<std::string, bool>        forceSlider;

    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;

        std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + 1));
        if (key.empty()) continue;

        if (key == "sliders") {
            for (auto const& n : splitTokens(value)) forceSlider[n] = true;
            continue;
        }
        if (key == "screen" || key == "profiles" || key == "prebuilt") continue;
        if (key.rfind("screen.", 0) == 0) {
            if (splitBy(key, '.').size() >= 3) {
                for (auto const& n : splitTokens(value)) screenOrder.push_back(n);
            }
            continue;
        }
        if (key.rfind("profile.", 0) == 0 || key == "profile") continue;

        // 只认 option.NAME 声明。
        if (key.rfind("option.", 0) != 0) continue;
        std::string name = trim(key.substr(7));
        if (name.empty()) continue;

        if (values.find(name) == values.end()) declared.push_back(name);
        values[name] = value;
    }

    std::vector<std::string> order;
    for (auto const& n : screenOrder) {
        if (values.count(n) && std::find(order.begin(), order.end(), n) == order.end()) order.push_back(n);
    }
    for (auto const& n : declared) {
        if (std::find(order.begin(), order.end(), n) == order.end()) order.push_back(n);
    }

    for (auto const& key : order) {
        auto it = values.find(key);
        if (it == values.end()) continue;

        ShaderParameter p;
        p.name  = key;
        p.macro = key;
        p.label = key;

        auto tokens = splitTokens(it->second);
        fillParameter(p, tokens);

        // sliders = NAME 强制为滑块：给一个常见默认区间（0..1）。
        if (forceSlider.count(key) && p.type != ParameterType::Slider) {
            p.type    = ParameterType::Slider;
            p.min     = 0.0;
            p.max     = 1.0;
            p.def     = 0.5;
            p.integer = false;
            p.current = "0.5";
        }

        params.push_back(std::move(p));
    }

    return params;
}

// 读取 `profiles = windows shading`：交给 lazurite 的 -p。
std::vector<std::string> ShaderPackStore::parseProfiles(std::filesystem::path const& propertiesFile) {
    std::vector<std::string> profiles;
    std::ifstream            in(propertiesFile);
    if (!in) return profiles;

    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (trim(t.substr(0, eq)) != "profiles") continue;
        profiles = splitTokens(t.substr(eq + 1));
        break;
    }
    return profiles;
}

// 从 <shaderpacks>/<名字>.txt 读回玩家上次的选项值。
void ShaderPackStore::applySettings(ShaderPack& pack) {
    if (pack.settingsPath.empty()) return;
    std::ifstream in(pack.settingsPath);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + 1));
        for (auto& p : pack.parameters) {
            if (p.name == key) {
                p.current = value;
                break;
            }
        }
    }
}

bool ShaderPackStore::saveSettings(ShaderPack const& pack) {
    if (pack.settingsPath.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(pack.settingsPath.parent_path(), ec);

    std::ofstream out(pack.settingsPath, std::ios::trunc);
    if (!out) return false;
    out << "# RenderDragonShaderLoader 保存的选项值（对标 OptiFine/Iris 的 shaderpack .txt）\n";
    out << "# 包: " << pack.name << "\n";
    for (auto const& p : pack.parameters) {
        out << p.name << "=" << p.current << "\n";
    }
    return out.good();
}

} // namespace renderdragon_shader_loader
