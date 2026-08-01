#include "WindowEffectPolicy.hpp"

#include <array>
#include <cstddef>

namespace {

enum class EWindowClassMatch {
    Exact,
    Prefix,
    Contains,
};

struct SWindowClassExclusion {
    std::string_view pattern;
    EWindowClassMatch match = EWindowClassMatch::Exact;
};

constexpr std::array<SWindowClassExclusion, 11> kAutomaticExclusions{{
    {
        .pattern = "realmheart",
        .match = EWindowClassMatch::Prefix,
    },
    {
        .pattern = "hyprlock",
        .match = EWindowClassMatch::Prefix,
    },
    {
        .pattern = "gamescope",
        .match = EWindowClassMatch::Prefix,
    },
    {
        .pattern = "steam_app_",
        .match = EWindowClassMatch::Prefix,
    },
    {
        .pattern = "xdg-desktop-portal",
        .match = EWindowClassMatch::Prefix,
    },
    {
        .pattern = "org.freedesktop.impl.portal.desktop",
        .match = EWindowClassMatch::Prefix,
    },
    {
        .pattern = "xwaylandvideobridge",
        .match = EWindowClassMatch::Contains,
    },
    {
        .pattern = "polkit",
        .match = EWindowClassMatch::Contains,
    },
    {
        .pattern = "pinentry",
        .match = EWindowClassMatch::Contains,
    },
    {
        .pattern = "askpass",
        .match = EWindowClassMatch::Contains,
    },
    {
        .pattern = "gcr-prompter",
        .match = EWindowClassMatch::Contains,
    },
}};

constexpr char asciiLower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

bool asciiEqualIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (asciiLower(left[index]) != asciiLower(right[index]))
            return false;
    }

    return true;
}

bool asciiStartsWithIgnoreCase(
    std::string_view value,
    std::string_view prefix
) noexcept {
    if (value.size() < prefix.size())
        return false;

    return asciiEqualIgnoreCase(value.substr(0, prefix.size()), prefix);
}

bool asciiContainsIgnoreCase(
    std::string_view value,
    std::string_view needle
) noexcept {
    if (needle.empty())
        return true;
    if (value.size() < needle.size())
        return false;

    for (std::size_t offset = 0; offset <= value.size() - needle.size(); ++offset) {
        if (asciiEqualIgnoreCase(value.substr(offset, needle.size()), needle))
            return true;
    }

    return false;
}

bool matchesExclusion(
    std::string_view windowClass,
    const SWindowClassExclusion& exclusion
) noexcept {
    switch (exclusion.match) {
        case EWindowClassMatch::Exact:
            return asciiEqualIgnoreCase(windowClass, exclusion.pattern);
        case EWindowClassMatch::Prefix:
            return asciiStartsWithIgnoreCase(windowClass, exclusion.pattern);
        case EWindowClassMatch::Contains:
            return asciiContainsIgnoreCase(windowClass, exclusion.pattern);
    }

    return false;
}

bool matchesText(
    std::string_view value,
    std::string_view pattern,
    EWindowEffectTextMatch match
) noexcept {
    switch (match) {
        case EWindowEffectTextMatch::Exact:
            return asciiEqualIgnoreCase(value, pattern);
        case EWindowEffectTextMatch::Prefix:
            return asciiStartsWithIgnoreCase(value, pattern);
        case EWindowEffectTextMatch::Contains:
            return asciiContainsIgnoreCase(value, pattern);
    }

    return false;
}

bool matchesRule(
    const SWindowEffectRule& rule,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept {
    if (rule.windowClass &&
        !matchesText(windowClass, *rule.windowClass, rule.classMatch)) {
        return false;
    }
    if (rule.windowTitle &&
        !matchesText(windowTitle, *rule.windowTitle, rule.titleMatch)) {
        return false;
    }
    return true;
}

EWindowEffectId automaticEffectForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle,
    bool opening
) noexcept {
    if (automaticWindowClassIsExcluded(windowClass))
        return EWindowEffectId::None;

    for (const auto& rule : config.rules) {
        if (!matchesRule(rule, windowClass, windowTitle))
            continue;

        const auto& assigned = opening ? rule.openEffect : rule.closeEffect;
        if (assigned)
            return *assigned;
    }

    return opening ? config.defaultOpenEffect : config.defaultCloseEffect;
}

} // namespace

bool automaticWindowClassIsExcluded(std::string_view windowClass) noexcept {
    if (windowClass.empty())
        return true;

    for (const auto& exclusion : kAutomaticExclusions) {
        if (matchesExclusion(windowClass, exclusion))
            return true;
    }

    return false;
}

EWindowEffectId automaticOpenEffectForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept {
    return automaticEffectForWindow(config, windowClass, windowTitle, true);
}

EWindowEffectId automaticCloseEffectForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept {
    return automaticEffectForWindow(config, windowClass, windowTitle, false);
}
