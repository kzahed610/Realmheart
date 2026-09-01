#pragma once

#include "core/DisplayTier.hpp"

#include <string_view>

namespace realmheart::core {

enum class MonitorAspectClass {
    Portrait,
    Standard,
    Ultrawide,
    SuperUltrawide,
};

struct MonitorContext {
    int index = 0;
    int x = 0;
    int y = 0;
    int logical_width = 1920;
    int logical_height = 1080;
    double scale = 1.0;
    DisplayTier layout_tier = DisplayTier::P1080;
    DisplayTier asset_tier = DisplayTier::P1080;
    MonitorAspectClass aspect = MonitorAspectClass::Standard;

    [[nodiscard]] bool valid() const noexcept {
        return logical_width > 0 && logical_height > 0 && scale > 0.0;
    }

    [[nodiscard]] int physical_width_hint() const noexcept;
    [[nodiscard]] int physical_height_hint() const noexcept;
};

[[nodiscard]] MonitorAspectClass monitor_aspect_class_for_logical_geometry(
    int width,
    int height
) noexcept;

[[nodiscard]] std::string_view monitor_aspect_class_name(
    MonitorAspectClass aspect
) noexcept;

[[nodiscard]] MonitorContext monitor_context_for_geometry(
    int index,
    int x,
    int y,
    int logical_width,
    int logical_height,
    double scale
) noexcept;

} // namespace realmheart::core
