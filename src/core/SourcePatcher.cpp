#include "core/SourcePatcher.h"

#include <algorithm>
#include <cctype>

namespace renderdragon_shader_loader {
namespace {

std::string_view trimLeft(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

bool isIdentChar(char c) {
    return std::isalnum((unsigned char)c) != 0 || c == '_';
}

// 若该行是 `#define NAME ...`，返回 NAME，否则返回空。
std::string_view defineName(std::string_view line) {
    auto t = trimLeft(line);
    constexpr std::string_view kw = "#define";
    if (t.size() <= kw.size() || !t.starts_with(kw)) return {};
    if (!std::isspace((unsigned char)t[kw.size()])) return {};

    std::size_t i = kw.size();
    while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
    std::size_t start = i;
    while (i < t.size() && isIdentChar(t[i])) ++i;
    return t.substr(start, i - start);
}

// 该行是否是针对 name 的 "未定义才用" 保护。
bool isGuardFor(std::string_view line, std::string_view name) {
    auto t = trimLeft(line);
    constexpr std::string_view nifndef = "#ifndef";
    if (t.starts_with(nifndef)) {
        std::size_t i = nifndef.size();
        while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
        return t.substr(i).starts_with(name)
            && (t.size() == i + name.size() || !isIdentChar(t[i + name.size()]));
    }
    if (t.starts_with("#if")) {
        return t.find("!defined") != std::string_view::npos
            && t.find(name) != std::string_view::npos;
    }
    return false;
}

bool isBlankOrComment(std::string_view line) {
    auto t = trimLeft(line);
    return t.empty() || t.starts_with("//") || t.starts_with("/*") || t.starts_with("*");
}

bool lineEndsWithContinuation(std::string_view line) {
    std::size_t end = line.size();
    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
    return end > 0 && line[end - 1] == '\\';
}

} // namespace

bool patchDefines(std::string& source, std::vector<std::string> const& names) {
    if (source.empty() || names.empty()) return false;

    std::vector<std::string_view> significant; // 已输出的、非空非注释行
    std::string                   out;
    out.reserve(source.size() + names.size() * 32);

    bool   changed = false;
    std::size_t pos = 0;

    while (pos <= source.size()) {
        if (pos == source.size()) break;
        std::size_t eol     = source.find('\n', pos);
        bool        hasNl   = eol != std::string::npos;
        std::size_t lineEnd = hasNl ? eol : source.size();
        std::string_view raw(source.data() + pos, lineEnd - pos);

        std::string_view line = raw;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        auto name = defineName(line);
        bool shouldWrap = false;
        if (!name.empty() && std::find(names.begin(), names.end(), name) != names.end()) {
            // 往前看最近的“有效”行：已经 #ifndef NAME 保护过就跳过。
            bool guarded = false;
            if (!significant.empty() && isGuardFor(significant.back(), name)) guarded = true;
            shouldWrap = !guarded;
        }

        if (shouldWrap) {
            out.append("#ifndef ");
            out.append(name);
            out.push_back('\n');
            out.append(raw);
            out.push_back('\n');

            // 多行 #define（行尾反斜杠）整段纳入保护。
            bool continued = lineEndsWithContinuation(line);
            while (continued && hasNl) {
                pos = lineEnd + 1;
                if (pos > source.size()) break;
                std::size_t e2 = source.find('\n', pos);
                bool        h2 = e2 != std::string::npos;
                std::size_t e2e = h2 ? e2 : source.size();
                std::string_view raw2(source.data() + pos, e2e - pos);
                std::string_view l2 = raw2;
                if (!l2.empty() && l2.back() == '\r') l2.remove_suffix(1);
                out.append(raw2);
                out.push_back('\n');
                continued = lineEndsWithContinuation(l2);
                lineEnd   = e2e;
                hasNl     = h2;
            }

            out.append("#endif\n");
            changed = true;
        } else {
            out.append(raw);
            if (hasNl) out.push_back('\n');
        }

        if (!isBlankOrComment(line)) {
            // 只保留最近的 2 行，够判断紧邻的 #ifndef 保护。
            if (significant.size() == 2) significant.erase(significant.begin());
            significant.push_back(line);
        }

        if (!hasNl) break;
        pos = lineEnd + 1;
    }

    if (changed) source = std::move(out);
    return changed;
}

bool patchDefines(std::string& source, std::string_view name) {
    return patchDefines(source, std::vector<std::string>{std::string(name)});
}

} // namespace renderdragon_shader_loader
