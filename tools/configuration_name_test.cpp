#include <iostream>
#include <string>
#include <string_view>

#include "ui/ConfigurationName.h"

using renderdragon_shader_loader::ui::normalizeConfigurationNameInput;

namespace {

int failures = 0;

void check(bool condition, char const* label) {
    if (condition) {
        std::cout << "[PASS] " << label << '\n';
    } else {
        std::cerr << "[FAIL] " << label << '\n';
        ++failures;
    }
}

std::string_view utf8(char8_t const* text) {
    auto const view = std::u8string_view(text);
    return {reinterpret_cast<char const*>(view.data()), view.size()};
}

} // namespace

int main() {
    check(normalizeConfigurationNameInput("Config 1") == "Config 1", "ASCII input");
    check(normalizeConfigurationNameInput("Sky-1") == "Sky-1", "English name");
    check(normalizeConfigurationNameInput(utf8(u8"中文配置")).empty(), "non-English input is ignored");
    check(normalizeConfigurationNameInput(utf8(u8"Sky天空1")) == "Sky1", "mixed input keeps ASCII only");
    check(
        normalizeConfigurationNameInput("Sky/Test:1") == "Sky-Test-1",
        "invalid path characters are sanitized"
    );
    check(
        normalizeConfigurationNameInput(std::string("A\0B\nC", 5)) == "ABC",
        "control characters are removed"
    );
    check(normalizeConfigurationNameInput("123456789", 5) == "12345", "input is bounded");

    if (failures != 0) return 1;
    std::cout << "PASS: configuration name input normalization\n";
    return 0;
}
