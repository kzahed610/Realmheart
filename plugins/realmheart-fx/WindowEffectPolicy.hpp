#pragma once

#include "WindowEffectConfig.hpp"

#include <cstdint>
#include <string_view>

[[nodiscard]] bool automaticWindowClassIsExcluded(
    std::string_view windowClass
) noexcept;

[[nodiscard]] const WindowEffectPool& automaticOpenEffectsForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept;

[[nodiscard]] const WindowEffectPool& automaticCloseEffectsForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept;

[[nodiscard]] std::string_view chooseWindowEffect(
    const WindowEffectPool& pool,
    std::uint64_t randomValue
) noexcept;
