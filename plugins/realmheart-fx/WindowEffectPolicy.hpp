#pragma once

#include "WindowEffectConfig.hpp"

#include <string_view>

[[nodiscard]] bool automaticWindowClassIsExcluded(
    std::string_view windowClass
) noexcept;

[[nodiscard]] EWindowEffectId automaticOpenEffectForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept;

[[nodiscard]] EWindowEffectId automaticCloseEffectForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept;
