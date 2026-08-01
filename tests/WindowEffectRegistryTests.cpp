#include "WindowEffectRegistry.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    assert(stream);
    stream << content;
}

} // namespace

int main() {
    const auto loaded = loadWindowEffectRegistry(REALMHEART_TEST_EFFECT_DIR);
    assert(loaded.success);
    assert(loaded.loadedEffects == 2U);
    assert(loaded.error.empty());

    const auto specs = windowEffectSpecs();
    assert(specs.size() == 3U);

    const auto* none = findWindowEffect("none");
    assert(none != nullptr);
    assert(windowEffectIsNone(*none));
    assert(none->fragmentShaderAsset.empty());
    assert(none->openDurationSeconds == 0.0F);
    assert(none->closeDurationSeconds == 0.0F);

    const auto* voidEffect = findWindowEffect("void");
    assert(voidEffect != nullptr);
    assert(voidEffect->displayName == "Realmheart Void");
    assert(voidEffect->fragmentShaderAsset == "void/void.frag");
    assert(std::abs(voidEffect->openDurationSeconds - 0.85F) < 0.0001F);
    assert(std::abs(voidEffect->closeDurationSeconds - 0.85F) < 0.0001F);
    assert(voidEffect->reversible);
    assert(windowEffectSupports(*voidEffect, EWindowEffectCapability::SourceTexture));
    assert(windowEffectSupports(*voidEffect, EWindowEffectCapability::Texture2D));
    assert(windowEffectSupports(*voidEffect, EWindowEffectCapability::ExternalTexture));
    assert(windowEffectSupports(*voidEffect, EWindowEffectCapability::RoundedSource));

    const auto* aetherSunder = findWindowEffect("aether-sunder");
    assert(aetherSunder != nullptr);
    assert(aetherSunder->displayName == "Aether Sunder");
    assert(aetherSunder->fragmentShaderAsset == "aether-sunder/aether-sunder.frag");
    assert(std::abs(aetherSunder->openDurationSeconds - 0.78F) < 0.0001F);
    assert(std::abs(aetherSunder->closeDurationSeconds - 0.78F) < 0.0001F);
    assert(aetherSunder->reversible);
    assert(findWindowEffect("missing-effect") == nullptr);

    const auto temporaryRoot = std::filesystem::temp_directory_path() /
        "realmheart-window-effect-manifest-tests";
    std::filesystem::remove_all(temporaryRoot);

    writeFile(
        temporaryRoot / "test-effect" / "effect.toml",
        R"TOML([effect]
id = "test-effect"
display_name = "Test Effect"
shader = "test.frag"
open_duration = 0.42
close_duration = 0.51
reversible = false

[capabilities]
external_texture = false
)TOML"
    );
    writeFile(temporaryRoot / "test-effect" / "test.frag", "shader");

    const auto custom = loadWindowEffectRegistry(temporaryRoot);
    assert(custom.success);
    assert(custom.loadedEffects == 1U);
    const auto* customEffect = findWindowEffect("test-effect");
    assert(customEffect != nullptr);
    assert(customEffect->displayName == "Test Effect");
    assert(!customEffect->reversible);
    assert(!windowEffectSupports(
        *customEffect,
        EWindowEffectCapability::ExternalTexture
    ));

    writeFile(
        temporaryRoot / "broken" / "effect.toml",
        R"TOML([effect]
id = "broken"
shader = "missing.frag"
open_duration = 0.5
close_duration = 0.5
)TOML"
    );
    const auto broken = loadWindowEffectRegistry(temporaryRoot);
    assert(!broken.success);
    assert(broken.error.find("shader file does not exist") != std::string::npos);
    assert(findWindowEffect("test-effect") != nullptr);

    std::filesystem::remove_all(temporaryRoot);
    return 0;
}
