#pragma once

#include "WindowEffectRegistry.hpp"

#include <string_view>

[[nodiscard]] bool automaticWindowClassIsExcluded(
    std::string_view windowClass
) noexcept;

[[nodiscard]] EWindowEffectId automaticOpenEffectForWindowClass(
    std::string_view windowClass
) noexcept;

[[nodiscard]] EWindowEffectId automaticCloseEffectForWindowClass(
    std::string_view windowClass
) noexcept;
