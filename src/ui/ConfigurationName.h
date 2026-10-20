#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace renderdragon_shader_loader::ui {

inline constexpr std::size_t kMaxConfigurationNameBytes = 96;

std::string normalizeConfigurationNameInput(
    std::string_view input,
    std::size_t maxBytes = kMaxConfigurationNameBytes
);

} // namespace renderdragon_shader_loader::ui
