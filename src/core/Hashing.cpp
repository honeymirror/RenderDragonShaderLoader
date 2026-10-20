#include "core/Hashing.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace renderdragon_shader_loader {
namespace {

constexpr std::uint64_t fnvOffset = 1469598103934665603ull;
constexpr std::uint64_t fnvPrime  = 1099511628211ull;

void mix(std::uint64_t& hash, std::string_view bytes) {
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= fnvPrime;
    }
}

std::string hex(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

bool extensionAllowed(std::filesystem::path const& file, std::vector<std::string> const& extensions) {
    if (extensions.empty()) return true;
    auto extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

} // namespace

std::string hashText(std::string_view text) { return hashBytes(text); }

std::string hashBytes(std::string_view bytes) {
    std::uint64_t hash = fnvOffset;
    mix(hash, bytes);
    return hex(hash);
}

std::string hashFile(std::filesystem::path const& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};

    std::uint64_t hash = fnvOffset;
    char          buffer[8192];
    while (in) {
        in.read(buffer, sizeof(buffer));
        auto count = in.gcount();
        if (count > 0) mix(hash, std::string_view(buffer, (std::size_t)count));
    }
    return hex(hash);
}

std::string hashFiles(std::vector<std::filesystem::path> const& files) {
    std::vector<std::filesystem::path> sorted = files;
    std::sort(sorted.begin(), sorted.end());

    std::uint64_t hash = fnvOffset;
    for (auto const& file : sorted) {
        auto relative = file.generic_string();
        mix(hash, relative);
        auto content = hashFile(file);
        mix(hash, content);
    }
    return hex(hash);
}

std::string hashDirectory(std::filesystem::path const& root, std::vector<std::string> const& extensions) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return {};

    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::recursive_directory_iterator(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             ec
         )) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (!extensionAllowed(entry.path(), extensions)) continue;
        files.push_back(entry.path());
    }

    std::sort(files.begin(), files.end());
    std::uint64_t hash = fnvOffset;
    for (auto const& file : files) {
        auto relative = std::filesystem::relative(file, root, ec);
        if (ec) {
            ec.clear();
            relative = file.filename();
        }
        mix(hash, relative.generic_string());
        mix(hash, hashFile(file));
    }
    return hex(hash);
}

} // namespace renderdragon_shader_loader
