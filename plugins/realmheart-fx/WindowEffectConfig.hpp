#pragma once

#include "WindowEffectRegistry.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class EWindowEffectTextMatch {
    Exact,
    Prefix,
    Contains,
};

using WindowEffectPool = std::vector<std::string>;

struct SWindowEffectRule {
    std::optional<std::string> windowClass;
    EWindowEffectTextMatch classMatch = EWindowEffectTextMatch::Exact;
    std::optional<std::string> windowTitle;
    EWindowEffectTextMatch titleMatch = EWindowEffectTextMatch::Contains;
    std::optional<WindowEffectPool> openEffects;
    std::optional<WindowEffectPool> closeEffects;
};

struct SWindowEffectConfig {
    WindowEffectPool defaultOpenEffects{"void"};
    WindowEffectPool defaultCloseEffects{"void"};
    std::vector<SWindowEffectRule> rules;
    std::filesystem::path sourcePath;
    bool loadedFromFile = false;
};

struct SWindowEffectConfigLoadResult {
    bool success = false;
    SWindowEffectConfig config;
    std::string error;
};

[[nodiscard]] SWindowEffectConfig builtInWindowEffectConfig();
[[nodiscard]] std::filesystem::path defaultWindowEffectConfigPath();
// Resolves the directory holding effect manifests + fragment shaders at
// runtime (env override -> self-location relative to the loaded .so ->
// compile-time constant as a last resort). Keeps the FX plugin working when
// the repo is copied to a different absolute path (e.g. a fresh account).
[[nodiscard]] std::filesystem::path defaultWindowEffectAssetRoot();
[[nodiscard]] SWindowEffectConfigLoadResult loadWindowEffectConfig(
    const std::filesystem::path& path
);
[[nodiscard]] std::string windowEffectPoolSummary(
    const WindowEffectPool& pool
);
[[nodiscard]] std::string windowEffectConfigSummary(
    const SWindowEffectConfig& config,
    const std::filesystem::path& requestedPath
);
