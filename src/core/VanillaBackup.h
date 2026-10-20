#pragma once

#include <filesystem>
#include <string>

namespace renderdragon_shader_loader {

// Copy every vanilla *.material.bin once. The snapshot is intentionally a
// directory so it can be inspected and restored without a custom archive tool.
bool ensureVanillaSnapshot(
    std::filesystem::path const& materialsDir,
    std::filesystem::path const& snapshotDir,
    std::string*                 error = nullptr
);

bool restoreVanillaSnapshot(
    std::filesystem::path const& snapshotDir,
    std::filesystem::path const& materialsDir,
    std::string*                 error = nullptr
);

// Export the snapshot as a zip that can be handed back to the user.
bool exportVanillaSnapshot(
    std::filesystem::path const& snapshotDir,
    std::filesystem::path const& outputZip,
    std::string*                 error = nullptr
);

} // namespace renderdragon_shader_loader
