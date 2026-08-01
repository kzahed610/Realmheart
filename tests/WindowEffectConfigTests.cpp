#include "WindowEffectConfig.hpp"

#include <algorithm>
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

bool hasEffect(const WindowEffectPool& pool, std::string_view effect) {
    return std::find(pool.begin(), pool.end(), effect) != pool.end();
}

} // namespace

int main() {
    const auto registry = loadWindowEffectRegistry(REALMHEART_TEST_EFFECT_DIR);
    assert(registry.success);

    const auto builtIn = builtInWindowEffectConfig();
    assert(builtIn.defaultOpenEffects.size() == 2U);
    assert(hasEffect(builtIn.defaultOpenEffects, "void"));
    assert(hasEffect(builtIn.defaultOpenEffects, "aether-sunder"));
    assert(builtIn.defaultCloseEffects == builtIn.defaultOpenEffects);
    assert(builtIn.rules.empty());
    assert(!builtIn.loadedFromFile);

    const auto directory = std::filesystem::temp_directory_path() /
        "realmheart-window-effect-config-tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const auto validPath = writeConfig(
        directory,
        "valid.toml",
        R"TOML(
# Arrays are random pools. @all expands to every registered non-none effect.
[windows]
open = ["aether-sunder", "void"]
close = "@all"

[[windows.rules]]
class = "org.kde."
class_match = "prefix"
open = "void"
close = ["none", "aether-sunder"]

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
    assert(valid.config.defaultOpenEffects == WindowEffectPool({
        "aether-sunder",
        "void",
    }));
    assert(valid.config.defaultCloseEffects.size() == 2U);
    assert(hasEffect(valid.config.defaultCloseEffects, "void"));
    assert(hasEffect(valid.config.defaultCloseEffects, "aether-sunder"));
    assert(valid.config.rules.size() == 2U);
    assert(valid.config.rules[0].windowClass == "org.kde.");
    assert(valid.config.rules[0].classMatch == EWindowEffectTextMatch::Prefix);
    assert(valid.config.rules[0].openEffects == WindowEffectPool({"void"}));
    assert(valid.config.rules[0].closeEffects == WindowEffectPool({
        "none",
        "aether-sunder",
    }));
    assert(valid.config.rules[1].windowTitle == "Picture-in-Picture");
    assert(valid.config.rules[1].titleMatch == EWindowEffectTextMatch::Contains);
    assert(valid.config.rules[1].openEffects == WindowEffectPool({"none"}));
    assert(!valid.config.rules[1].closeEffects);

    const auto unknownEffectPath = writeConfig(
        directory,
        "unknown-effect.toml",
        R"TOML([windows]
open = ["void", "does-not-exist"]
close = "void"
)TOML"
    );
    const auto unknownEffect = loadWindowEffectConfig(unknownEffectPath);
    assert(!unknownEffect.success);
    assert(unknownEffect.error.find("unknown registered effect") != std::string::npos);

    const auto duplicateEffectPath = writeConfig(
        directory,
        "duplicate-effect.toml",
        R"TOML([windows]
open = ["void", "void"]
close = "void"
)TOML"
    );
    const auto duplicateEffect = loadWindowEffectConfig(duplicateEffectPath);
    assert(!duplicateEffect.success);
    assert(duplicateEffect.error.find("duplicate effect in pool") != std::string::npos);

    const auto allInsideArrayPath = writeConfig(
        directory,
        "all-inside-array.toml",
        R"TOML([windows]
open = ["@all", "none"]
close = "void"
)TOML"
    );
    const auto allInsideArray = loadWindowEffectConfig(allInsideArrayPath);
    assert(!allInsideArray.success);
    assert(allInsideArray.error.find("@all must be used by itself") != std::string::npos);

    const auto emptyPoolPath = writeConfig(
        directory,
        "empty-pool.toml",
        R"TOML([windows]
open = []
close = "void"
)TOML"
    );
    const auto emptyPool = loadWindowEffectConfig(emptyPoolPath);
    assert(!emptyPool.success);
    assert(emptyPool.error.find("effect pool cannot be empty") != std::string::npos);

    const auto invalidRulePath = writeConfig(
        directory,
        "invalid-rule.toml",
        R"TOML([windows]
open = "@all"
close = "@all"

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
    assert(summary.find("open=[aether-sunder,void]") != std::string::npos);
    assert(summary.find("close=[aether-sunder,void]") != std::string::npos);
    assert(summary.find("rules=2") != std::string::npos);

    std::filesystem::remove_all(directory);
    return 0;
}
