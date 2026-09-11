#pragma once

#include "core/MonitorContext.hpp"

namespace realmheart::ui::events {

// Geometry is derived entirely from the monitor's logical dimensions. The
// Event Surface deliberately has no authored pixel-width contract: it should
// preserve the same readable proportions on laptops, QHD/4K panels, portrait
// displays, and ultrawides.
struct EventSurfaceGeometry {
    int surface_width = 0;
    int max_surface_height = 0;
    int details_max_height = 0;
    int top_margin = 0;

    constexpr bool operator==(const EventSurfaceGeometry&) const noexcept = default;
};

[[nodiscard]] EventSurfaceGeometry event_surface_geometry_for_monitor(
    const core::MonitorContext& monitor
) noexcept;

} // namespace realmheart::ui::events
