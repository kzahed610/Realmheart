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

struct SWindowEffectRule {
    std::optional<std::string> windowClass;
    EWindowEffectTextMatch classMatch = EWindowEffectTextMatch::Exact;
    std::optional<std::string> windowTitle;
    EWindowEffectTextMatch titleMatch = EWindowEffectTextMatch::Contains;
    std::optional<EWindowEffectId> openEffect;
    std::optional<EWindowEffectId> closeEffect;
};

struct SWindowEffectConfig {
    EWindowEffectId defaultOpenEffect = EWindowEffectId::Void;
    EWindowEffectId defaultCloseEffect = EWindowEffectId::Void;
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
[[nodiscard]] SWindowEffectConfigLoadResult loadWindowEffectConfig(
    const std::filesystem::path& path
);
[[nodiscard]] std::string windowEffectConfigSummary(
    const SWindowEffectConfig& config,
    const std::filesystem::path& requestedPath
);
