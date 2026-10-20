#include "core/Hotkey.h"

#include <cctype>
#include <string_view>

namespace renderdragon_shader_loader {
namespace {

// 只列这份表，避免包含 <Windows.h>（数字就是 VK_* 的值）。
struct NamedKey {
    char const* name;
    int         vk;
};

constexpr NamedKey kNamedKeys[] = {
    {"BACKSPACE", 0x08},
    {"TAB", 0x09},
    {"ENTER", 0x0D},
    {"RETURN", 0x0D},
    {"SHIFT", 0x10},
    {"CTRL", 0x11},
    {"CONTROL", 0x11},
    {"ALT", 0x12},
    {"CAPSLOCK", 0x14},
    {"ESC", 0x1B},
    {"ESCAPE", 0x1B},
    {"SPACE", 0x20},
    {"PGUP", 0x21},
    {"PAGEUP", 0x21},
    {"PGDN", 0x22},
    {"PAGEDOWN", 0x22},
    {"END", 0x23},
    {"HOME", 0x24},
    {"LEFT", 0x25},
    {"UP", 0x26},
    {"RIGHT", 0x27},
    {"DOWN", 0x28},
    {"INSERT", 0x2D},
    {"DELETE", 0x2E},
    {"DEL", 0x2E},
    {"TILDE", 0xC0},
    {"BACKTICK", 0xC0},
    {"MINUS", 0xBD},
    {"EQUALS", 0xBB},
    {"LBRACKET", 0xDB},
    {"RBRACKET", 0xDD},
    {"BACKSLASH", 0xDC},
    {"SEMICOLON", 0xBA},
    {"APOSTROPHE", 0xDE},
    {"COMMA", 0xBC},
    {"PERIOD", 0xBE},
    {"SLASH", 0xBF},
};

constexpr NamedKey kOemKeys[] = {
    {"`", 0xC0},
    {"~", 0xC0},
    {"-", 0xBD},
    {"=", 0xBB},
    {"[", 0xDB},
    {"]", 0xDD},
    {"\\", 0xDC},
    {";", 0xBA},
    {"'", 0xDE},
    {",", 0xBC},
    {".", 0xBE},
    {"/", 0xBF},
};

} // namespace

std::string normalizeHotkeyName(std::string const& raw) {
    std::string s;
    for (char ch : raw) {
        if (std::isspace((unsigned char)ch)) continue;
        s.push_back((char)std::toupper((unsigned char)ch));
    }
    return s;
}

int hotkeyVkFromName(std::string const& raw) {
    std::string const s = normalizeHotkeyName(raw);
    if (s.empty() || s == "NONE" || s == "OFF" || s == "-" || s == "DISABLED") return kHotkeyNone;

    if (s.size() == 1) {
        char const c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; // VK_A..Z / VK_0..9 就是字符本身
        for (auto const& oem : kOemKeys) {
            if (s == oem.name) return oem.vk;
        }
        return kHotkeyUnknown;
    }

    // F1..F24
    if (s[0] == 'F') {
        int  n        = 0;
        bool allDigit = s.size() > 1;
        for (std::size_t i = 1; i < s.size(); ++i) {
            if (std::isdigit((unsigned char)s[i])) n = n * 10 + (s[i] - '0');
            else {
                allDigit = false;
                break;
            }
        }
        if (allDigit && n >= 1 && n <= 24) return 0x70 + (n - 1); // VK_F1
    }

    // NUMPAD0..9
    if (s.rfind("NUMPAD", 0) == 0 && s.size() == 7 && std::isdigit((unsigned char)s[6])) {
        return 0x60 + (s[6] - '0');
    }

    for (auto const& named : kNamedKeys) {
        if (s == named.name) return named.vk;
    }
    return kHotkeyUnknown;
}

std::string hotkeyNameFromVk(int vk) {
    if (vk <= 0) return {};

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= 0x70 && vk <= 0x70 + 23) return "F" + std::to_string(vk - 0x70 + 1); // F1..F24
    if (vk >= 0x60 && vk <= 0x69) return "NUMPAD" + std::to_string(vk - 0x60);

    for (auto const& named : kNamedKeys) {
        if (named.vk == vk) return named.name;
    }
    for (auto const& oem : kOemKeys) {
        if (oem.vk == vk) return oem.name;
    }
    return {};
}

} // namespace renderdragon_shader_loader
