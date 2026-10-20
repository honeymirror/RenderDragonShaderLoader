#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace renderdragon_shader_loader {

struct NativeIOIssue {
    std::string kind;
    std::string code;
    std::string material;
    std::string pass;
    std::string buffer;
    std::string message;
    std::string overrideId;
    std::string reason;
};

struct NativeIOMappingRecord {
    std::string section;
    std::string id;
    std::string material;
    std::string pass;
    std::string source;
    std::string target;
    std::string mode;
    std::string reason;
};

struct NativeIOReport {
    bool        available = false;
    bool        ok        = false;
    int         schemaVersion = 0;
    std::size_t candidateMaterials = 0;
    std::size_t engineMaterials = 0;
    std::size_t commonMaterials = 0;
    std::size_t engineMaterialsUnused = 0;
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t overridesApplied = 0;
    std::size_t overridesUnused = 0;
    std::vector<NativeIOIssue> issues;
    std::vector<NativeIOIssue> overriddenIssues;
    std::vector<NativeIOMappingRecord> appliedMappings;
    std::string json;

    [[nodiscard]] std::size_t errorIssueCount() const {
        std::size_t count = 0;
        for (auto const& item : issues) {
            if (item.kind == "error") ++count;
        }
        return count;
    }

    [[nodiscard]] std::size_t warningIssueCount() const {
        std::size_t count = 0;
        for (auto const& item : issues) {
            if (item.kind == "warning") ++count;
        }
        return count;
    }
};

} // namespace renderdragon_shader_loader
