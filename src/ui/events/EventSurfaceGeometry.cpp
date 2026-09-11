#include "ui/events/EventSurfaceGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::events {
namespace {

int fraction_of(int extent, double fraction) noexcept {
    if (extent <= 0 || !std::isfinite(fraction) || fraction <= 0.0) return 0;
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(extent) * fraction)));
}

} // namespace

EventSurfaceGeometry event_surface_geometry_for_monitor(
    const core::MonitorContext& monitor
) noexcept {
    const int width = std::max(monitor.logical_width, 1);
    const int height = std::max(monitor.logical_height, 1);
    const int short_edge = std::min(width, height);

    // Reading width follows the monitor's short edge, so an ultrawide does not
    // turn a diagnostic card into a metre-long log line. Portrait displays use
    // more of their available width while preserving comfortable edge space.
    const double readable_short_edge_fraction =
        monitor.aspect == core::MonitorAspectClass::Portrait ? 0.82 : 0.66;
    const int readable_width = fraction_of(short_edge, readable_short_edge_fraction);
    const int edge_safe_width = fraction_of(width, 0.86);

    EventSurfaceGeometry geometry;
    geometry.surface_width = std::min(readable_width, edge_safe_width);
    geometry.max_surface_height = fraction_of(height, 0.70);
    geometry.details_max_height = fraction_of(height, 0.30);
    geometry.top_margin = fraction_of(short_edge, 0.015);
    return geometry;
}

} // namespace realmheart::ui::events
