#pragma once

#include "WindowEffectConfig.hpp"

#include <string_view>

[[nodiscard]] bool automaticWindowClassIsExcluded(
    std::string_view windowClass
) noexcept;

[[nodiscard]] std::string_view automaticOpenEffectForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept;

[[nodiscard]] std::string_view automaticCloseEffectForWindow(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle
) noexcept;
