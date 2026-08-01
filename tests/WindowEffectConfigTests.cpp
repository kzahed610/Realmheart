#include "WindowEffectConfig.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path writeConfig(
    const std::filesystem::path& directory,
    std::string_view name,
    std::string_view content
) {
    const auto path = directory / name;
    std::ofstream stream(path);
    assert(stream);
    stream << content;
    return path;
}

} // namespace

int main() {
    const auto registry = loadWindowEffectRegistry(REALMHEART_TEST_EFFECT_DIR);
    assert(registry.success);

    const auto builtIn = builtInWindowEffectConfig();
    assert(builtIn.defaultOpenEffect == std::string{"void"});
    assert(builtIn.defaultCloseEffect == std::string{"void"});
    assert(builtIn.rules.size() == 1U);
    assert(builtIn.rules.front().windowClass == "kitty");
    assert(builtIn.rules.front().openEffect == std::string{"aether-sunder"});
    assert(builtIn.rules.front().closeEffect == std::string{"aether-sunder"});
    assert(!builtIn.loadedFromFile);

    const auto directory = std::filesystem::temp_directory_path() /
        "realmheart-window-effect-config-tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const auto validPath = writeConfig(
        directory,
        "valid.toml",
        R"TOML(
# Defaults apply to every eligible normal window.
[windows]
open = "aether-sunder"
close = "void"

[[windows.rules]]
class = "org.kde."
class_match = "prefix"
open = "void"
close = "none"

[[windows.rules]]
title = "Picture-in-Picture"
title_match = "contains"
open = "none"
)TOML"
    );

    const auto valid = loadWindowEffectConfig(validPath);
    assert(valid.success);
    assert(valid.error.empty());
    assert(valid.config.loadedFromFile);
    assert(valid.config.sourcePath == validPath);
    assert(valid.config.defaultOpenEffect == std::string{"aether-sunder"});
    assert(valid.config.defaultCloseEffect == std::string{"void"});
    assert(valid.config.rules.size() == 2U);
    assert(valid.config.rules[0].windowClass == "org.kde.");
    assert(valid.config.rules[0].classMatch == EWindowEffectTextMatch::Prefix);
    assert(valid.config.rules[0].openEffect == std::string{"void"});
    assert(valid.config.rules[0].closeEffect == std::string{"none"});
    assert(valid.config.rules[1].windowTitle == "Picture-in-Picture");
    assert(valid.config.rules[1].titleMatch == EWindowEffectTextMatch::Contains);
    assert(valid.config.rules[1].openEffect == std::string{"none"});
    assert(!valid.config.rules[1].closeEffect);

    const auto unknownEffectPath = writeConfig(
        directory,
        "unknown-effect.toml",
        R"TOML([windows]
open = "does-not-exist"
close = "void"
)TOML"
    );
    const auto unknownEffect = loadWindowEffectConfig(unknownEffectPath);
    assert(!unknownEffect.success);
    assert(unknownEffect.error.find("unknown registered effect") != std::string::npos);

    const auto invalidRulePath = writeConfig(
        directory,
        "invalid-rule.toml",
        R"TOML([windows]
open = "void"
close = "void"

[[windows.rules]]
class = "kitty"
)TOML"
    );
    const auto invalidRule = loadWindowEffectConfig(invalidRulePath);
    assert(!invalidRule.success);
    assert(invalidRule.error.find("needs open and/or close") != std::string::npos);

    const auto duplicatePath = writeConfig(
        directory,
        "duplicate.toml",
        R"TOML([windows]
open = "void"
open = "aether-sunder"
)TOML"
    );
    const auto duplicate = loadWindowEffectConfig(duplicatePath);
    assert(!duplicate.success);
    assert(duplicate.error.find("duplicate key") != std::string::npos);

    const auto missing = loadWindowEffectConfig(directory / "missing.toml");
    assert(!missing.success);
    assert(missing.error.find("could not open") != std::string::npos);

    const auto summary = windowEffectConfigSummary(valid.config, validPath);
    assert(summary.find("source=file") != std::string::npos);
    assert(summary.find("open=aether-sunder") != std::string::npos);
    assert(summary.find("close=void") != std::string::npos);
    assert(summary.find("rules=2") != std::string::npos);

    std::filesystem::remove_all(directory);
    return 0;
}
