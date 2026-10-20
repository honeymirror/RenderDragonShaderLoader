#include "core/MaterialValidation.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace renderdragon_shader_loader {
namespace {

std::uint32_t read32(std::ifstream& in) {
    unsigned char bytes[4]{};
    in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    return (std::uint32_t)bytes[0] | ((std::uint32_t)bytes[1] << 8) | ((std::uint32_t)bytes[2] << 16)
         | ((std::uint32_t)bytes[3] << 24);
}

} // namespace

bool readMaterialHeader(std::filesystem::path const& file, std::uint32_t& magic, std::uint32_t& version) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    magic = read32(in);
    // The definition string is NUL-terminated; version 25 starts at offset 0x33.
    in.seekg(0x33, std::ios::beg);
    if (!in) return false;
    version = read32(in);
    return in.good();
}

bool validateMaterialFile(std::filesystem::path const& file, std::string* error) {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!readMaterialHeader(file, magic, version)) {
        if (error) *error = "cannot read material header: " + file.string();
        return false;
    }
    if (magic != kMaterialMagic) {
        if (error) *error = "invalid material magic: " + file.filename().string();
        return false;
    }
    if (version < kMaterialMinFormatVersion || version > kMaterialFormatVersion) {
        if (error) {
            std::error_code sizeEc;
            auto            size = std::filesystem::file_size(file, sizeEc);
            *error = "unsupported material format version " + std::to_string(version) + " (expected "
                   + std::to_string(kMaterialMinFormatVersion) + ".."
                   + std::to_string(kMaterialFormatVersion) + "): " + file.filename().string()
                   + " [size=" + std::to_string(sizeEc ? 0 : size) + "]";
        }
        return false;
    }
    return true;
}

bool validateMaterialDir(std::filesystem::path const& dir, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        if (error) *error = "material output directory does not exist: " + dir.string();
        return false;
    }

    std::size_t count = 0;
    for (auto const& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().filename().string().ends_with(".material.bin")) {
            ++count;
            if (!validateMaterialFile(entry.path(), error)) return false;
        }
    }
    if (count == 0) {
        if (error) *error = "material output directory has no .material.bin";
        return false;
    }
    return true;
}

bool validateCompiledOutput(std::filesystem::path const& dir, std::string* error) {
    if (!validateMaterialDir(dir, error)) return false;

    std::ifstream in(dir / "meta.json", std::ios::binary);
    if (!in) return true; // shipped authored presets may legitimately have no meta.json.
    try {
        nlohmann::json meta;
        in >> meta;
        if (!meta.contains("materials") || !meta["materials"].is_array()) {
            if (error) *error = "meta.json has no materials list";
            return false;
        }
        for (auto const& item : meta["materials"]) {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) continue;
            auto file = dir / item["name"].get<std::string>();
            if (!std::filesystem::is_regular_file(file)) {
                if (error) *error = "meta.json lists a missing material: " + file.filename().string();
                return false;
            }
            if (item.contains("size") && item["size"].is_number_unsigned()) {
                auto expected = item["size"].get<std::uint64_t>();
                std::error_code ec;
                auto actual = std::filesystem::file_size(file, ec);
                if (ec || actual != expected) {
                    if (error) *error = "material size mismatch: " + file.filename().string();
                    return false;
                }
            }
        }
    } catch (...) {
        if (error) *error = "cannot parse meta.json";
        return false;
    }
    return true;
}

} // namespace renderdragon_shader_loader
