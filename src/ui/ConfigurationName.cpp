#include "ui/ConfigurationName.h"

namespace renderdragon_shader_loader::ui {
namespace {

bool invalidPathCharacter(unsigned char value) {
    constexpr std::string_view invalid = "\\/:*?\"<>|";
    return value < 0x80 && invalid.find(static_cast<char>(value)) != std::string_view::npos;
}

} // namespace

std::string normalizeConfigurationNameInput(std::string_view input, std::size_t maxBytes) {
    std::string result;
    result.reserve(input.size() < maxBytes ? input.size() : maxBytes);

    for (std::size_t i = 0; i < input.size();) {
        auto const value = static_cast<unsigned char>(input[i]);
        ++i;
        if (value < 0x20 || value >= 0x7F) continue;
        if (result.size() + 1 > maxBytes) break;
        result.push_back(invalidPathCharacter(value) ? '-' : static_cast<char>(value));
    }
    return result;
}

} // namespace renderdragon_shader_loader::ui
