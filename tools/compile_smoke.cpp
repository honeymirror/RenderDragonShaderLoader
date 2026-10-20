// Usage:
//   compile_smoke.exe <projectDir> <outputDir> <vanillaDir> [MACRO=VALUE ...]
//
// This is an offline smoke test for the same CompilerService used by the mod.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/CompilerService.h"
#include "core/PackMetadata.h"

#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: compile_smoke <projectDir> <outputDir> <vanillaDir> [MACRO=VALUE ...]\n";
        return 2;
    }
    if (auto check = std::getenv("RDL_CHECK_STALE"); check != nullptr && *check != '\0') {
        bool const stale = renderdragon_shader_loader::compiledOutputNeedsRebuild(argv[2]);
        std::cout << "stale=" << (stale ? "true" : "false") << "\n";
        return stale ? 0 : 1;
    }

    renderdragon_shader_loader::CompilerSettings settings;
    settings.workingDir        = std::filesystem::current_path();
    settings.outputDir         = argv[2];
    settings.vanillaDir        = argv[3];
    settings.enableCompilation = true;
    settings.skipValidation   = true;
    if (auto command = std::getenv("RDL_LAZURITE_COMMAND"); command != nullptr && *command != '\0') {
        settings.lazuriteCommand = command;
    }
    if (auto timeout = std::getenv("RDL_TIMEOUT_SECONDS"); timeout != nullptr && *timeout != '\0') {
        settings.timeoutSeconds = std::max(1, std::atoi(timeout));
    }
    if (auto command = std::getenv("RDL_LAZURITE_ALBITE_COMMAND"); command != nullptr && *command != '\0') {
        settings.lazuriteAlbiteCommand = command;
    }
    if (auto path = std::getenv("RDL_ALBITE_PATH"); path != nullptr && *path != '\0') {
        settings.albitePath = path;
    }
    if (auto args = std::getenv("RDL_ALBITE_ARGS"); args != nullptr) {
        settings.albiteArgs = args;
    }
    auto shaderc               = std::filesystem::current_path() / "toolchain" / "shaderc.exe";
    if (std::filesystem::is_regular_file(shaderc)) settings.shadercPath = shaderc.string();

    renderdragon_shader_loader::CompileOptions options;
    options.outputDir = argv[2];
    options.packName  = std::filesystem::path(argv[1]).filename().string();
    options.preset    = "smoke";
    options.platform  = "Direct3D_SM65";
    options.cacheDir  = std::filesystem::path(argv[2]).parent_path() / "cache" / options.packName;
    auto packOptions  = renderdragon_shader_loader::loadPackOptions(argv[1]);
    auto parameters   = renderdragon_shader_loader::optionsToShaderParameters(packOptions);
    std::string uniformError;
    if (!renderdragon_shader_loader::collectUniformPatches(
            parameters,
            options.uniformPatches,
            uniformError
        )) {
        std::cerr << uniformError << "\n";
        return 1;
    }
    if (auto backend = std::getenv("RDL_BACKEND"); backend != nullptr && *backend != '\0') {
        options.backend = backend;
    }
    if (auto materials = std::getenv("RDL_MATERIALS"); materials != nullptr && *materials != '\0') {
        std::string value = materials;
        std::size_t start = 0;
        while (start <= value.size()) {
            auto end = value.find(',', start);
            auto item = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!item.empty()) options.materials.push_back(item);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (auto profiles = std::getenv("RDL_PROFILES"); profiles != nullptr && *profiles != '\0') {
        std::string value = profiles;
        std::size_t start = 0;
        while (start <= value.size()) {
            auto end = value.find(',', start);
            auto item = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!item.empty()) options.profiles.push_back(item);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (auto staged = std::getenv("RDL_STAGED"); staged != nullptr && *staged != '\0') {
        options.stagedPublish = std::string(staged) != "0";
    }
    if (auto settle = std::getenv("RDL_STAGE_SETTLE_MS"); settle != nullptr && *settle != '\0') {
        options.stageSettleMs = std::max(0, std::atoi(settle));
    }
    for (int i = 4; i < argc; ++i) options.macros.emplace_back(argv[i]);

    auto& service = renderdragon_shader_loader::CompilerService::getInstance();
    service.configure(std::move(settings));
    service.setOnFinished([](renderdragon_shader_loader::CompileFinishedInfo const& info) {
        std::cout << "EVENT stage=" << info.stageIndex << "/" << info.stageCount
                  << " complete=" << (info.completeOutput ? "true" : "false")
                  << " materials=" << info.materialCount
                  << " message=" << info.message << "\n";
    });
    if (auto ioOnly = std::getenv("RDL_IO_ONLY"); ioOnly != nullptr && *ioOnly != '\0') {
        int repeat = 1;
        if (auto value = std::getenv("RDL_IO_REPEAT"); value != nullptr && *value != '\0') {
            repeat = std::max(1, std::atoi(value));
        }
        std::string error;
        renderdragon_shader_loader::NativeIOReport report;
        bool allOk = true;
        for (int pass = 1; pass <= repeat; ++pass) {
            error.clear();
            report = {};
            bool const passOk = service.validateNativeIO(argv[1], argv[2], report, error);
            allOk = allOk && passOk;
            std::cout << "io_pass=" << pass << "/" << repeat << " "
                      << (passOk ? "ok" : "failed")
                      << " report=" << (report.available ? "true" : "false")
                      << " errors=" << report.errors
                      << " warnings=" << report.warnings
                      << " overridden=" << report.overriddenIssues.size()
                      << " mappings=" << report.appliedMappings.size()
                      << " json_bytes=" << report.json.size() << "\n";
        }
        std::cout << "io_only=" << (allOk ? "ok" : "failed");
        std::cout << " report=" << (report.available ? "true" : "false")
                  << " errors=" << report.errors
                  << " warnings=" << report.warnings
                  << " overridden=" << report.overriddenIssues.size()
                  << " mappings=" << report.appliedMappings.size()
                  << " json_bytes=" << report.json.size();
        if (!error.empty()) std::cout << " error=" << error;
        std::cout << "\n";
        for (auto const& issue : report.issues) {
            std::cout << "issue " << issue.kind << " [" << issue.code << "] "
                      << issue.material;
            if (!issue.pass.empty()) std::cout << "/" << issue.pass;
            if (!issue.buffer.empty()) std::cout << "/" << issue.buffer;
            std::cout << ": " << issue.message << "\n";
        }
        for (auto const& mapping : report.appliedMappings) {
            std::cout << "mapping " << mapping.section << " [" << mapping.id << "] "
                      << mapping.source << " -> " << mapping.target << "\n";
        }
        return allOk ? 0 : 1;
    }
    if (!service.compile(argv[1], std::move(options))) {
        std::cerr << "compile refused: " << service.lastMessage() << "\n";
        return 1;
    }
    while (service.running()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (auto const& line : service.log()) std::cout << line << "\n";
    auto status = service.status();
    std::cout << "status=" << (int)status << " message=" << service.lastMessage() << "\n";
    return status == renderdragon_shader_loader::CompileStatus::Succeeded ? 0 : 1;
}
