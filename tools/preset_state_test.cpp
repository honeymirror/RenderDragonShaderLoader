#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "core/ShaderPack.h"

using namespace renderdragon_shader_loader;

namespace {

int fail(std::filesystem::path const& root, std::string const& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return 1;
}

void writeFile(std::filesystem::path const& path, std::string_view contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

} // namespace

int main() {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto root = std::filesystem::temp_directory_path()
              / ("rdloader-preset-state-" + std::to_string(stamp));
    auto packs = root / "shaderpacks";
    auto generated = root / "generated";
    auto presets = root / "presets";
    auto packRoot = packs / "Demo";

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    writeFile(packRoot / "shaders.properties", "profile=windows\n");
    writeFile(
        packRoot / "options.json",
        R"({
  "options": [
    {
      "name": "TEST_VALUE",
      "macro": "TEST_VALUE",
      "type": "slider",
      "min": 0,
      "max": 10,
      "default": 1,
      "step": 1
    }
  ]
})"
    );
    writeFile(
        packRoot / "rdloader.pack.json",
        R"({
  "packVersion": 1,
  "name": "Preset state test",
  "defaultPreset": "default",
  "presets": [
    { "id": "default", "label": "Default", "bins": "missing-output" }
  ]
})"
    );

    auto& store = ShaderPackStore::getInstance();
    store.scan(packs, generated, presets);
    auto const* pack = store.find("Demo");
    if (pack == nullptr) return fail(root, "pack was not discovered");
    if (pack->currentPreset != "default") {
        return fail(root, "initial scan did not select the implicit default preset");
    }

    // Simulate the player compiling into the writable preset. The preset did not
    // exist during the first scan, so ensureWritablePreset must remember it.
    writeFile(presets / "Demo" / "custom" / "compiled" / "Demo.material.bin", "test");
    auto const* writable = store.ensureWritablePreset("Demo");
    if (writable == nullptr) return fail(root, "ensureWritablePreset returned null");
    if (writable->currentPreset != "custom") {
        return fail(root, "ensureWritablePreset did not select custom");
    }

    store.scan(packs, generated, presets);
    pack = store.find("Demo");
    if (pack == nullptr) return fail(root, "pack disappeared after rescan");
    if (pack->currentPreset != "custom") {
        return fail(
            root,
            "rescan restored stale preset '" + pack->currentPreset + "' instead of custom"
        );
    }
    if (pack->binsDir.filename() != "compiled"
        || pack->binsDir.parent_path().filename() != "custom") {
        return fail(root, "rescan selected a different output directory");
    }

    // Deleting the active local preset must reset parameters to the fallback
    // preset and clear the deleted settings path, so a later save cannot write
    // stale values into the author/default configuration.
    if (!store.createLocalPreset("Demo", "scratch")) {
        return fail(root, "could not create scratch preset");
    }
    if (!store.setParameterValueByName("Demo", "TEST_VALUE", "9")) {
        return fail(root, "could not set scratch parameter");
    }
    pack = store.find("Demo");
    if (pack == nullptr || pack->currentPreset != "scratch") {
        return fail(root, "scratch preset was not activated");
    }
    if (!ShaderPackStore::saveSettings(*pack)) {
        return fail(root, "could not save scratch settings");
    }
    if (!store.setParameterValueByName("Demo", "TEST_VALUE", "5")) {
        return fail(root, "could not create unsaved scratch value");
    }
    if (!store.selectPreset("Demo", "scratch")) {
        return fail(root, "could not reload active preset");
    }
    pack = store.find("Demo");
    if (pack == nullptr || pack->parameters.front().current != "9") {
        return fail(root, "reset did not restore the saved active preset value");
    }
    if (!store.deleteLocalPreset("Demo", "scratch")) {
        return fail(root, "could not delete active local preset");
    }
    pack = store.find("Demo");
    if (pack == nullptr) return fail(root, "pack disappeared after preset deletion");
    if (pack->currentPreset != "default") {
        return fail(root, "deletion did not return to the author default preset");
    }
    if (pack->parameters.empty() || pack->parameters.front().current != "1") {
        return fail(root, "deleted preset values leaked into the fallback preset");
    }
    if (!pack->settingsPath.empty()) {
        return fail(root, "deleted settings path remained active after fallback");
    }
    if (ShaderPackStore::saveSettings(*pack)) {
        return fail(root, "fallback without settingsPath unexpectedly wrote a settings file");
    }
    if (std::filesystem::exists(presets / "Demo" / "default" / "settings.txt", ec)) {
        return fail(root, "fallback preset was polluted by a settings file");
    }

    std::filesystem::remove_all(root, ec);
    std::cout << "PASS: writable preset and deletion reset remain stable\n";
    return 0;
}
