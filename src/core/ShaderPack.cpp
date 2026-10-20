#include "core/ShaderPack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <nlohmann/json.hpp>

#include "core/MaterialValidation.h"

namespace renderdragon_shader_loader {
namespace {

std::string_view trimView(std::string_view s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 玩家选中的预设：只活在进程里，但要跨"重开面板触发的重扫"。
// loadPresets 每次重扫都会跑一遍，如果不记住玩家的选择，就会被 manifest 的
// 默认预设抢回去（适配包的 shipped default 没有产物 ⇒ 又触发一次编译）。
std::set<std::string>& presetPreferences() {
    static std::set<std::string> preferences;
    return preferences;
}

std::string presetPreferenceKey(std::string_view packName, std::string_view presetId) {
    return std::string(packName) + "/" + std::string(presetId);
}

bool findPresetPreference(std::string_view packName, std::string& out) {
    std::string const prefix = std::string(packName) + "/";
    for (auto const& key : presetPreferences()) {
        if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0) {
            out = key.substr(prefix.size());
            return true;
        }
    }
    return false;
}

void rememberPresetPreference(std::string_view packName, std::string_view presetId) {
    if (packName.empty() || presetId.empty()) return;
    auto& preferences = presetPreferences();
    std::string const prefix = std::string(packName) + "/";
    for (auto it = preferences.begin(); it != preferences.end();) {
        if (it->compare(0, prefix.size(), prefix) == 0) it = preferences.erase(it);
        else ++it;
    }
    preferences.insert(presetPreferenceKey(packName, presetId));
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

bool isSourceExt(std::filesystem::path const& file) {
    auto ext = toLower(file.extension().string());
    return ext == ".glsl" || ext == ".sc" || ext == ".h" || ext == ".inc";
}

std::vector<std::string> parseBracketValues(std::string const& line) {
    auto open  = line.find('[');
    auto close = line.find(']', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos) return {};
    return splitTokens(std::string_view(line).substr(open + 1, close - open - 1));
}

std::string firstIdentifier(std::string_view value) {
    std::size_t start = value.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    std::size_t end = start;
    while (end < value.size()
           && (std::isalnum((unsigned char)value[end]) != 0 || value[end] == '_')) {
        ++end;
    }
    return std::string(value.substr(start, end - start));
}

std::vector<ShaderParameter> scanSourceParameters(std::filesystem::path const& packRoot) {
    std::map<std::string, std::string> defineValue;
    std::map<std::string, bool>        defineDefault;
    std::set<std::string>              referenced;
    std::map<std::string, std::vector<std::string>> valueLists;

    std::error_code ec;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(
             packRoot,
             std::filesystem::directory_options::skip_permission_denied,
             ec
         )) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || !isSourceExt(entry.path())) continue;
        std::ifstream in(entry.path());
        std::string   line;
        while (std::getline(in, line)) {
            auto t = trim(line);
            if (t.empty()) continue;
            if (t.rfind("#ifdef", 0) == 0 || t.rfind("#ifndef", 0) == 0) {
                auto name = firstIdentifier(std::string_view(t).substr(6));
                if (!name.empty()) referenced.insert(name);
                continue;
            }
            if (t.rfind("#if", 0) == 0 && t.find("defined") != std::string::npos) {
                auto open = t.find('(');
                auto close = t.find(')', open == std::string::npos ? 0 : open);
                if (open != std::string::npos && close != std::string::npos) {
                    auto name = firstIdentifier(std::string_view(t).substr(open + 1, close - open - 1));
                    if (!name.empty()) referenced.insert(name);
                }
            }

            bool commented = t.rfind("//#define", 0) == 0;
            if (!(commented ? t.rfind("//#define", 0) == 0 : t.rfind("#define", 0) == 0)) continue;
            auto afterKeyword = t.find("#define") + 7;
            while (afterKeyword < t.size() && std::isspace((unsigned char)t[afterKeyword])) ++afterKeyword;
            auto name = firstIdentifier(std::string_view(t).substr(afterKeyword));
            if (name.empty()) continue;
            defineDefault[name] = !commented;
            auto value = trim(t.substr(afterKeyword + name.size()));
            defineValue[name] = value;
            auto list = parseBracketValues(t);
            if (!list.empty()) valueLists[name] = list;
        }
    }

    std::vector<ShaderParameter> result;
    for (auto const& name : referenced) {
        auto it = defineDefault.find(name);
        if (it == defineDefault.end()) continue;
        ShaderParameter parameter;
        parameter.name  = name;
        parameter.macro = name;
        parameter.label = name;
        auto listIt = valueLists.find(name);
        if (listIt != valueLists.end()) {
            parameter.type   = ParameterType::Enum;
            parameter.values = listIt->second;
            parameter.current = it->second ? (parameter.values.empty() ? "ON" : parameter.values.front()) : "OFF";
        } else {
            parameter.type    = ParameterType::Bool;
            parameter.values  = {"OFF", "ON"};
            parameter.current = it->second ? "ON" : "OFF";
        }
        result.push_back(std::move(parameter));
    }
    return result;
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

std::size_t countMaterialBins(std::filesystem::path const& dir) {
    std::error_code ec;
    std::size_t     count = 0;
    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename().string().ends_with(".material.bin")) ++count;
    }
    return count;
}

bool materialDirCompatible(std::filesystem::path const& dir) {
    std::error_code ec;
    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (!entry.path().filename().string().ends_with(".material.bin")) continue;
        if (!validateMaterialFile(entry.path())) return false;
    }
    return true;
}

std::string readBuiltAt(std::filesystem::path const& dir) {
    std::ifstream in(dir / "meta.json");
    if (!in) return {};
    try {
        nlohmann::json meta;
        in >> meta;
        if (meta.contains("createdUtc") && meta["createdUtc"].is_number_integer()) {
            return std::to_string(meta["createdUtc"].get<long long>());
        }
    } catch (...) {
    }
    return {};
}

void copyFileIfMissing(std::filesystem::path const& from, std::filesystem::path const& to) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(from, ec) || std::filesystem::exists(to, ec)) return;
    std::filesystem::create_directories(to.parent_path(), ec);
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
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

// zip 只在首次或来源变化时解压。marker 记录文件大小、mtime、创建时间和
// Windows file id；这样删除后重新创建同名 zip 也会被识别为新来源。
bool ensureExtracted(
    std::filesystem::path const& zip,
    std::filesystem::path const& dest,
    bool&                        sourceChanged
) {
    sourceChanged = false;
    std::error_code ec;
    auto            size = std::filesystem::file_size(zip, ec);
    if (ec) return false;
    auto            zipTime = std::filesystem::last_write_time(zip, ec);
    if (ec) return false;
    auto mtime = (long long)zipTime.time_since_epoch().count();

    std::ostringstream identity;
    identity << size << ':' << mtime;
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        zip.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(handle, &info)) {
            identity << ':' << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh << ':'
                     << info.nFileIndexLow << ':' << info.ftCreationTime.dwHighDateTime << ':'
                     << info.ftCreationTime.dwLowDateTime;
        }
        CloseHandle(handle);
    }
#endif
    std::string const currentIdentity = identity.str();

    auto marker = dest / ".source.zip";
    if (std::filesystem::is_regular_file(marker, ec)) {
        std::ifstream in(marker);
        std::string   stored;
        std::getline(in, stored);
        if (stored == currentIdentity) return true;

        // Migrate the legacy mtime-only marker without clearing existing configs
        // on the first scan after upgrading.
        bool legacyNumeric = !stored.empty()
                          && std::all_of(stored.begin(), stored.end(), [](unsigned char c) {
                                 return std::isdigit(c) != 0;
                             });
        if (legacyNumeric) {
            try {
                if (std::stoll(stored) == mtime) {
                    std::ofstream out(marker, std::ios::trunc);
                    out << currentIdentity;
                    return true;
                }
            } catch (...) {
            }
        }
    }

    sourceChanged = true;
    std::filesystem::remove_all(dest, ec);
    std::filesystem::create_directories(dest, ec);
    if (!extractZip(zip, dest)) return false;

    std::filesystem::create_directories(dest, ec);
    std::ofstream out(marker);
    out << currentIdentity;
    return true;
}

bool hasPackMarker(std::filesystem::path const& dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "project.json", ec)
        || std::filesystem::is_regular_file(dir / "shaders.properties", ec)
        || std::filesystem::is_regular_file(dir / "rdloader.pack.json", ec)
        || std::filesystem::is_regular_file(dir / "options.json", ec)
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
//   2) 含 subpacks/<名>/ 的只保留**名字最大的那个变体**；
//   3) 同级按路径排序，结果稳定。
//
// 递归是为了兼容各种外壳目录：产物可能位于
// `<container>/<pack>/render/renderer/materials`，硬编码 renderer/materials 会漏。
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

// 预编译包（作者发行的资源包）：loadPack 会把材质拷到 generated/<包>/compiled，
// 但真正被使用的是 presets/<包>/<预设>/compiled。之前两处不一致，导致
// "包里明明有 54 个材质，模组却说没有产物，还把包根目录当工程乱编"。
// 扫描完成后把基础 renderer/materials 同步进当前预设目录（不含可选 subpack，
// 保持作者默认外观）。
void syncPrebuiltIntoActivePreset(ShaderPack& pack) {
    if (pack.prebuiltDir.empty() || pack.binsDir.empty()) return;

    std::error_code ec;
    auto source = std::filesystem::weakly_canonical(pack.prebuiltDir, ec);
    if (ec || source.empty()) return;
    auto target = std::filesystem::weakly_canonical(pack.binsDir, ec);
    if (ec) {
        ec.clear();
        target = pack.binsDir;
    }
    auto sourceText = source.string();
    auto targetText = target.string();
    // 目标就是源目录时不要清空源（例如预设直接指向包内 renderer/materials）。
    if (targetText.rfind(sourceText, 0) == 0) return;

    // 已经同步过就跳过：这套拷贝有上百 MB，每次打开面板都重拷会卡 0.5-1s。
    std::size_t sourceCount = countMaterialBins(source);
    if (sourceCount > 0 && sourceCount == countMaterialBins(target)) {
        bool same = true;
        for (auto const& entry : std::filesystem::directory_iterator(source, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            auto name = entry.path().filename().string();
            if (!name.ends_with(".material.bin")) continue;
            auto destination = target / name;
            std::error_code sizeEc;
            if (!std::filesystem::is_regular_file(destination, sizeEc)
                || std::filesystem::file_size(destination, sizeEc)
                       != std::filesystem::file_size(entry.path(), sizeEc)) {
                same = false;
                break;
            }
        }
        if (same) return;
    }

    std::filesystem::remove_all(target, ec);
    std::filesystem::create_directories(target, ec);

    std::size_t copied = 0;
    for (auto const& entry : std::filesystem::directory_iterator(source, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto name = entry.path().filename().string();
        if (!name.ends_with(".material.bin")) continue;
        std::filesystem::copy_file(
            entry.path(),
            target / name,
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
        if (!ec) ++copied;
    }
    if (copied == 0) return;

    for (auto& preset : pack.presets) {
        if (preset.id != pack.currentPreset) continue;
        preset.compiled     = true;
        preset.compatible   = materialDirCompatible(pack.binsDir);
        preset.materialCount = copied;
        break;
    }
    pack.renderMode = detectRenderMode(pack.binsDir);
}

// zip 里常有一层外壳目录（如 `<project>-main/`），或者包自己的项目在 src/ 下。
// 找到真正能交给 lazurite 的项目根。
std::filesystem::path normalizeSourceDir(std::filesystem::path const& dir) {
    std::error_code ec;
    if (hasPackMarker(dir)) return dir;

    // 包自己的项目在 src/（仓库式布局）
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

bool ShaderPackStore::setParameterValueByName(
    std::string_view packName,
    std::string_view macro,
    std::string      value
) {
    for (auto& pack : mPacks) {
        if (pack.name != packName) continue;
        for (auto& parameter : pack.parameters) {
            if (parameter.macro != macro && parameter.name != macro) continue;
            parameter.current = std::move(value);
            return true;
        }
        return false;
    }
    return false;
}

bool ShaderPackStore::selectPreset(std::string_view packName, std::string_view presetId) {
    for (auto& pack : mPacks) {
        if (pack.name != packName) continue;
        return activatePreset(pack, presetId);
    }
    return false;
}

ShaderPack const* ShaderPackStore::ensureWritablePreset(std::string_view packName) {
    for (auto& pack : mPacks) {
        if (pack.name != packName) continue;

        for (auto const& preset : pack.presets) {
            if (preset.id == pack.currentPreset && preset.origin == "local") return &pack;
        }

        std::error_code ec;
        auto localRoot = mPresetsRoot / pack.name;
        auto presetDir = localRoot / "custom";
        std::filesystem::create_directories(presetDir, ec);

        auto existing = std::find_if(
            pack.presets.begin(),
            pack.presets.end(),
            [](ShaderPack::Preset const& preset) { return preset.id == "custom"; }
        );
        if (existing == pack.presets.end()) {
            ShaderPack::Preset preset;
            preset.id           = "custom";
            preset.label        = "Custom";
            preset.origin       = "local";
            preset.settingsPath = presetDir / "settings.txt";
            preset.binsDir      = presetDir / "compiled";
            pack.presets.push_back(std::move(preset));
            existing = std::prev(pack.presets.end());
        }
        pack.currentPreset = existing->id;
        pack.settingsPath  = existing->settingsPath;
        pack.binsDir       = existing->binsDir;
        pack.renderMode    = detectRenderMode(pack.binsDir);
        // This switch is made immediately before compiling. Remember it here too,
        // otherwise the next rescan can restore an older preference that points at
        // an empty shipped preset and make the compatibility page report no output.
        rememberPresetPreference(pack.name, existing->id);
        return &pack;
    }
    return nullptr;
}

namespace {
bool isValidPresetId(std::string_view id) {
    if (id.empty() || id == "." || id == "..") return false;
    return id.find_first_of("\\/:*?\"<>|") == std::string_view::npos;
}
} // namespace

bool ShaderPackStore::createLocalPreset(
    std::string_view packName,
    std::string_view presetId,
    std::string_view basePreset
) {
    if (!isValidPresetId(presetId)) return false;
    for (auto& pack : mPacks) {
        if (pack.name != packName) continue;

        for (auto const& preset : pack.presets) {
            if (preset.id == presetId) return activatePreset(pack, presetId);
        }

        std::error_code ec;
        auto presetDir = mPresetsRoot / pack.name / std::string(presetId);
        std::filesystem::create_directories(presetDir, ec);

        ShaderPack::Preset preset;
        preset.id          = std::string(presetId);
        preset.label       = preset.id;
        preset.origin      = "local";
        preset.settingsPath = presetDir / "settings.txt";
        preset.binsDir     = presetDir / "compiled";

        if (!basePreset.empty()) {
            for (auto const& source : pack.presets) {
                if (source.id != basePreset) continue;
                copyFileIfMissing(source.settingsPath, preset.settingsPath);
                break;
            }
        }
        if (!std::filesystem::is_regular_file(preset.settingsPath, ec)) {
            std::ofstream out(preset.settingsPath, std::ios::trunc);
            if (out) {
                for (auto const& parameter : pack.parameters) {
                    out << parameter.name << "=" << parameter.current << "\n";
                }
            }
        }

        pack.presets.push_back(std::move(preset));
        return activatePreset(pack, presetId);
    }
    return false;
}

bool ShaderPackStore::renameLocalPreset(
    std::string_view packName,
    std::string_view presetId,
    std::string_view newId
) {
    if (!isValidPresetId(newId)) return false;
    for (auto& pack : mPacks) {
        if (pack.name != packName) continue;
        for (auto& preset : pack.presets) {
            if (preset.id != presetId || preset.origin != "local") continue;
            if (std::any_of(pack.presets.begin(), pack.presets.end(), [&](ShaderPack::Preset const& other) {
                    return &other != &preset && other.id == newId;
                })) {
                return false;
            }
            auto oldDir = preset.settingsPath.parent_path();
            auto newDir = mPresetsRoot / pack.name / std::string(newId);
            std::error_code ec;
            std::filesystem::rename(oldDir, newDir, ec);
            if (ec) return false;
            auto wasCurrent = pack.currentPreset == preset.id;
            preset.id = std::string(newId);
            preset.label = preset.id;
            preset.settingsPath = newDir / "settings.txt";
            preset.binsDir = newDir / "compiled";
            if (wasCurrent) {
                pack.currentPreset = preset.id;
                pack.settingsPath = preset.settingsPath;
                pack.binsDir = preset.binsDir;
                rememberPresetPreference(pack.name, preset.id);
            }
            return true;
        }
    }
    return false;
}

bool ShaderPackStore::deleteLocalPreset(std::string_view packName, std::string_view presetId) {
    for (auto& pack : mPacks) {
        if (pack.name != packName) continue;
        if (pack.presets.size() <= 1) return false;
        for (auto it = pack.presets.begin(); it != pack.presets.end(); ++it) {
            if (it->id != presetId || it->origin != "local") continue;
            std::error_code ec;
            auto const presetDir = it->settingsPath.parent_path();
            if (!presetDir.empty()) std::filesystem::remove_all(presetDir, ec);
            if (ec || (!presetDir.empty() && std::filesystem::exists(presetDir, ec))) return false;
            bool wasCurrent = pack.currentPreset == it->id;
            pack.presets.erase(it);
            if (wasCurrent) {
                if (!activatePreset(pack, pack.presets.front().id)) {
                    pack.currentPreset.clear();
                    pack.settingsPath.clear();
                    applySettings(pack);
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

bool ShaderPackStore::exportPreset(
    std::string_view packName,
    std::string_view presetId,
    std::filesystem::path const& outputFile
) {
    for (auto const& pack : mPacks) {
        if (pack.name != packName) continue;
        for (auto const& preset : pack.presets) {
            if (preset.id != presetId) continue;
            std::error_code ec;
            std::filesystem::create_directories(outputFile.parent_path(), ec);
            if (std::filesystem::is_regular_file(preset.settingsPath, ec)) {
                std::filesystem::copy_file(
                    preset.settingsPath,
                    outputFile,
                    std::filesystem::copy_options::overwrite_existing,
                    ec
                );
                return !ec;
            }
            std::ofstream out(outputFile, std::ios::trunc);
            if (!out) return false;
            for (auto const& parameter : pack.parameters) {
                out << parameter.name << "=" << parameter.current << "\n";
            }
            return out.good();
        }
    }
    return false;
}

bool ShaderPackStore::activatePreset(ShaderPack& pack, std::string_view presetId) {
    for (auto const& preset : pack.presets) {
        if (preset.id != presetId) continue;
        pack.currentPreset = preset.id;
        pack.settingsPath = preset.settingsPath;
        if (!preset.binsDir.empty()) pack.binsDir = preset.binsDir;
        applySettings(pack);
        pack.renderMode = detectRenderMode(pack.binsDir);
        rememberPresetPreference(pack.name, preset.id);
        return true;
    }
    return false;
}

void ShaderPackStore::loadPresets(
    ShaderPack&                  pack,
    std::filesystem::path const& presetsRoot,
    std::filesystem::path const& legacySettings
) {
    std::error_code ec;
    auto localRoot = presetsRoot / pack.name;
    // 纯预编译包（作者发行的资源包）没有源码，也没有自定义参数/预设可言。
    // v0.1.1 之前它们直接使用 generated/<包>/compiled（loadPack 已把包内
    // 现成材质拷进去）。preset 层是后加的，不能让它把这些包改指向空目录。
    bool const prebuiltPack = !pack.prebuiltDir.empty();

    // Migrate the old shaderpacks/<pack>.txt into the implicit default preset.
    if (std::filesystem::is_regular_file(legacySettings, ec)) {
        auto migrated = localRoot / "default" / "settings.txt";
        copyFileIfMissing(legacySettings, migrated);
    }

    if (pack.manifest.valid) {
        for (auto const& authored : pack.manifest.presets) {
            // A local directory with the same id is the player's editable copy.
            // Do not replace it with the author's read-only shipped preset.
            if (std::filesystem::is_directory(localRoot / authored.id, ec)) continue;

            ShaderPack::Preset preset;
            preset.id       = authored.id;
            preset.label    = authored.label.empty() ? authored.id : authored.label;
            preset.origin   = "shipped";
            preset.binsDir  = authored.bins.empty() ? std::filesystem::path{} : pack.packageRoot / authored.bins;
            preset.settingsPath = authored.settings.empty() ? std::filesystem::path{}
                                                             : pack.packageRoot / authored.settings;
            // Settings-only author presets reuse the pack's shared compiled output.
            if (preset.binsDir.empty()) preset.binsDir = pack.binsDir;
            if (std::filesystem::is_directory(preset.binsDir, ec)) {
                preset.compiled      = hasMaterialBin(preset.binsDir);
                preset.compatible    = preset.compiled && materialDirCompatible(preset.binsDir);
                preset.materialCount = countMaterialBins(preset.binsDir);
                preset.builtAt       = readBuiltAt(preset.binsDir);
            }
            pack.presets.push_back(std::move(preset));
        }
    }

    // Local presets live under data/presets/<pack>/<preset>/compiled.
    if (!prebuiltPack && std::filesystem::is_directory(localRoot, ec)) {
        for (auto const& entry : std::filesystem::directory_iterator(localRoot, ec)) {
            if (!entry.is_directory(ec)) continue;
            auto id = entry.path().filename().string();
            if (id.empty() || id[0] == '.' || id == "cache") continue;
            if (std::any_of(pack.presets.begin(), pack.presets.end(), [&](ShaderPack::Preset const& preset) {
                    return preset.id == id;
                })) {
                continue;
            }

            ShaderPack::Preset preset;
            preset.id          = id;
            preset.label       = id;
            preset.origin      = "local";
            preset.settingsPath = entry.path() / "settings.txt";
            preset.binsDir     = entry.path() / "compiled";
            preset.compiled    = hasMaterialBin(preset.binsDir);
            preset.compatible  = preset.compiled && materialDirCompatible(preset.binsDir);
            preset.materialCount = countMaterialBins(preset.binsDir);
            preset.builtAt       = readBuiltAt(preset.binsDir);
            pack.presets.push_back(std::move(preset));
        }
    }

    if (pack.presets.empty()) {
        ShaderPack::Preset preset;
        preset.id       = "default";
        preset.label    = "Default";
        preset.origin   = "local";
        preset.settingsPath = localRoot / "default" / "settings.txt";
        preset.binsDir  = prebuiltPack
                            ? pack.binsDir
                            : (hasMaterialBin(pack.binsDir) ? pack.binsDir
                                                            : preset.settingsPath.parent_path() / "compiled");
        preset.compiled = hasMaterialBin(preset.binsDir);
        preset.compatible = preset.compiled && materialDirCompatible(preset.binsDir);
        preset.materialCount = countMaterialBins(preset.binsDir);
        preset.builtAt       = readBuiltAt(preset.binsDir);
        pack.presets.push_back(std::move(preset));
    }

    // 配置文件显示顺序固定：default 最前，shipped 次之，本地配置最后按名称排序。
    // 文件系统目录枚举顺序不稳定，如果不排序，重扫后 default 可能被挤到列表末尾。
    std::stable_sort(pack.presets.begin(), pack.presets.end(), [](ShaderPack::Preset const& a, ShaderPack::Preset const& b) {
        bool const aDefault = a.id == "default";
        bool const bDefault = b.id == "default";
        if (aDefault != bDefault) return aDefault;
        bool const aShipped = a.origin == "shipped";
        bool const bShipped = b.origin == "shipped";
        if (aShipped != bShipped) return aShipped;
        if (aShipped && bShipped) return false; // preserve the author's declared order
        return a.id < b.id;
    });

    // 预设选择顺序（每次重扫面板都会走一遍，所以必须稳定）：
    //   1) 本地预设里已经编译产物的（玩家真正在用的那份，优先取最近构建的）；
    //   2) 包内自带且有产物的 shipped 预设（按 manifest 默认 / 声明顺序）；
    //   3) 都没有就退回第一个。
    //
    // 之前的顺序是"manifest 默认预设优先"，而适配包的 shipped default 指向
    // 包里并不存在的 presets/default/renderer/materials。于是每次重开面板，
    // currentPreset 都被打回那个空预设 → validateCompiledOutput 失败 → 又编译一遍。
    std::string selected;
    std::string remembered;
    if (findPresetPreference(pack.name, remembered)) {
        for (auto const& preset : pack.presets) {
            if (preset.id == remembered) {
                selected = preset.id;
                break;
            }
        }
    }

    long long newestBuilt = -1;
    for (auto const& preset : pack.presets) {
        if (!selected.empty()) break; // 玩家上次选的还在，就不用再挑
        if (preset.origin != "local" || !preset.compiled) continue;
        long long built = 0;
        try {
            built = std::stoll(preset.builtAt);
        } catch (...) {
            built = 0; // 没有 meta.json 也当作"有产物但时间未知"
        }
        if (built <= newestBuilt) continue;
        selected    = preset.id;
        newestBuilt = built;
    }
    if (selected.empty() && !pack.manifest.defaultPreset.empty()) {
        for (auto const& preset : pack.presets) {
            if (preset.id == pack.manifest.defaultPreset) {
                selected = preset.id;
                break;
            }
        }
    }
    if (selected.empty()) {
        for (auto const& preset : pack.presets) {
            if (preset.compiled) {
                selected = preset.id;
                break;
            }
        }
    }
    if (selected.empty()) selected = pack.presets.front().id;
    activatePreset(pack, selected);
}

void ShaderPackStore::scan(
    std::filesystem::path const& packsRoot,
    std::filesystem::path const& generatedRoot,
    std::filesystem::path const& presetsRoot
) {
    mPacks.clear();
    std::error_code ec;
    auto const effectivePresetsRoot = presetsRoot.empty() ? packsRoot.parent_path() / "presets" : presetsRoot;
    mPresetsRoot = effectivePresetsRoot;
    std::set<std::string> sourcePackNames;

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
            sourcePackNames.insert(fileName);

            // 目录形式的包：直接用原目录做源，产物在 generated/<名字>/compiled/。
            auto bins = generatedRoot / fileName / "compiled";
            auto legacySettings = packsRoot / (fileName + ".txt");
            auto pack = loadPack(entry.path(), {}, bins, legacySettings);
            loadPresets(pack, effectivePresetsRoot, legacySettings);
            applySettings(pack);
            syncPrebuiltIntoActivePreset(pack);
            mPacks.push_back(std::move(pack));
            continue;
        }

        if (!entry.is_regular_file(ec)) continue;

        auto path = entry.path();
        auto ext  = toLower(path.extension().string());
        if (ext == ".mcpack") {
            // .mcpack 其实就是 zip（玩家从浏览器/文件管理器拖进来时常常是这个名字）。
            // 这里自动把后缀改成 .zip 再按 zip 处理，行为与窗口拖放导入一致。
            auto            target = path;
            std::error_code renameEc;
            target.replace_extension(".zip");
            if (!std::filesystem::exists(target, renameEc)) {
                std::filesystem::rename(path, target, renameEc);
                if (!renameEc) path = target;
            }
            ext = ".zip";
        }
        if (ext != ".zip") continue;

        // .zip 形式的包：解压到 generated/<名字>/src/ 作为工作副本。
        auto name = path.stem().string();
        sourcePackNames.insert(name);
        auto src  = generatedRoot / name / "src";
        bool sourceChanged = false;
        bool const extracted = ensureExtracted(path, src, sourceChanged);
        if (sourceChanged) {
            std::filesystem::remove_all(effectivePresetsRoot / name, ec);
            std::filesystem::remove_all(generatedRoot / name / "compiled", ec);
            std::filesystem::remove_all(generatedRoot / name / "cache", ec);
        }
        if (!extracted) {
            // 解压失败（空 zip / 坏 zip）也照样列出来：Iris 也是这样——玩家能看到自己放进去的
            // 包，真要编译/使用时再报错。以前这里直接 continue，导致刚放进去的空 zip 根本
            // 不出现在列表里（实测就是这个问题）。
            auto legacySettings = packsRoot / (path.filename().string() + ".txt");
            auto pack = loadPack(
                src,
                path,
                generatedRoot / name / "compiled",
                legacySettings,
                src
            );
            loadPresets(pack, effectivePresetsRoot, legacySettings);
            applySettings(pack);
            syncPrebuiltIntoActivePreset(pack);
            mPacks.push_back(std::move(pack));
            continue;
        }

        auto bins = generatedRoot / name / "compiled";
        auto legacySettings = packsRoot / (path.filename().string() + ".txt");
        auto pack = loadPack(normalizeSourceDir(src), path, bins, legacySettings, src);
        loadPresets(pack, effectivePresetsRoot, legacySettings);
        applySettings(pack);
        syncPrebuiltIntoActivePreset(pack);
        mPacks.push_back(std::move(pack));
    }

    // Removed shader packs must not leave stale configurations behind. Keep a
    // preset root only while a corresponding shaderpack entry still exists.
    if (std::filesystem::is_directory(effectivePresetsRoot, ec)) {
        for (auto const& entry : std::filesystem::directory_iterator(effectivePresetsRoot, ec)) {
            if (!entry.is_directory(ec)) continue;
            auto const name = entry.path().filename().string();
            if (!sourcePackNames.contains(name)) {
                std::filesystem::remove_all(entry.path(), ec);
            }
        }
    }
}

ShaderRenderMode detectRenderMode(std::filesystem::path const& binsDir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(binsDir, ec)) return ShaderRenderMode::Forward;

    // 判据：延迟渲染专属的 pass 名字。26.40 vanilla 材质里属于这一类的有
    // DeferredShading / DeferredShadingBase / DeferredIndirectSpecular / DeferredTiledMixedRes /
    // DeferredMixedResolution(+Caustics/LightingPrepass/SurfaceRadiance) / DeferredWater /
    // SkyProbeDeferredShading；共有的前缀就是 "Deferred"，所以按前缀收口。
    // （大型延迟包通常包含多个 Deferred* 产物，前向包通常没有。）
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
    // 缓存按"包"共享，不按预设分家：预设之间通常只差几个滑条，材质级缓存键
    // 是内容寻址的（材质 + 平台 + profile + 该材质用到的宏 + 源码指纹 + 编译器），
    // 所以切预设时没受影响的材质可以直接复用，不必整包重编。
    pack.cacheDir = binsDir.parent_path().parent_path() / "cache";
    pack.settingsPath = settingsPath;

    // 名字优先用 zip / 目录名，与 settingsPath、binsDir 保持一致。
    if (!packFile.empty()) {
        pack.name = packFile.stem().string();
    } else {
        pack.name = sourceDir.filename().string();
    }

    pack.packageRoot = sourceDir;
    pack.manifest     = loadPackManifest(sourceDir);
    if (!pack.manifest.valid && !containerDir.empty()) {
        pack.manifest = loadPackManifest(containerDir);
        if (pack.manifest.valid) pack.packageRoot = containerDir;
    }
    pack.options = loadPackOptions(pack.packageRoot);

    pack.profiles = parseProfiles(pack.packageRoot / "shaders.properties");
    if (pack.profiles.empty() && !pack.manifest.profiles.empty()) pack.profiles = pack.manifest.profiles;
    pack.parameters = pack.options.valid ? optionsToShaderParameters(pack.options)
                                         : parseParameters(pack.packageRoot / "shaders.properties");
    if (pack.parameters.empty()) {
        pack.parameters = scanSourceParameters(pack.packageRoot);
    }

    // 玩家上次调好的选项（.txt）覆盖默认值。
    applySettings(pack);

    // 包里已经有编译好的 *.material.bin（发行版资源包）：直接拷到 binsDir，无需编译。
    //
    // 发行版一般是资源包布局：renderer/materials/ 是基础材质，subpacks/<名>/renderer/materials/
    // 是变体（例如有无体积云的两套产物会有多个同名文件但内容不同）。
    // 所以这里按 **基础 → 各 subpack（按名字排序，后面的覆盖前面的）** 依次拷，同名以后者为准；
    // `shaders.properties` 里的 `prebuilt = <相对路径>` 优先级最高，第一个拷。
    // 现成产物的搜索根：包自己的工程目录，加上（zip 包的）解压根。
    // 仓库式包（如 `<project>-main`）的发行产物在 `pack/`，是工程目录 `src/` 的兄弟目录，
    // 只从 sourceDir 找会全都漏掉 —— 实测就是"包里明明有 .material.bin，模组却说没有产物"。
    std::vector<std::filesystem::path> prebuiltRoots;
    prebuiltRoots.push_back(sourceDir);
    if (!containerDir.empty() && containerDir != sourceDir) {
        std::error_code e;
        if (std::filesystem::is_directory(containerDir, e)) prebuiltRoots.push_back(containerDir);
    }

    if (!pack.manifest.valid) {
        for (auto const& root : prebuiltRoots) {
            pack.prebuiltDir = findPrebuiltMaterials(root);
            if (!pack.prebuiltDir.empty()) break;
        }
    }
    if (!pack.manifest.valid && !hasMaterialBin(pack.binsDir)) {
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

        p.defaultValue = p.current;
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
    for (auto& p : pack.parameters) {
        if (!p.defaultValue.empty()) {
            p.current = p.defaultValue;
        } else if (p.type == ParameterType::Bool || p.type == ParameterType::Enum) {
            if (!p.values.empty()) p.current = p.values.front();
        } else if (p.type == ParameterType::Slider) {
            p.current = std::to_string(p.def);
        }
    }

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
