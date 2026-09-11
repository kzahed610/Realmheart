#include "core/MonitorContext.hpp"
#include "ui/events/EventSurfaceGeometry.hpp"

#include <cassert>

using realmheart::core::monitor_context_for_geometry;
using realmheart::ui::events::event_surface_geometry_for_monitor;

int main() {
    const auto full_hd = monitor_context_for_geometry(0, 0, 0, 1920, 1080, 1.0);
    const auto qhd = monitor_context_for_geometry(0, 0, 0, 2560, 1440, 1.0);
    const auto ultrawide = monitor_context_for_geometry(0, 0, 0, 3440, 1440, 1.0);
    const auto portrait = monitor_context_for_geometry(0, 0, 0, 1080, 1920, 1.0);

    const auto hd_geometry = event_surface_geometry_for_monitor(full_hd);
    const auto qhd_geometry = event_surface_geometry_for_monitor(qhd);
    const auto ultrawide_geometry = event_surface_geometry_for_monitor(ultrawide);
    const auto portrait_geometry = event_surface_geometry_for_monitor(portrait);

    assert(hd_geometry.surface_width > 0);
    assert(hd_geometry.max_surface_height > 0);
    assert(hd_geometry.details_max_height > 0);
    assert(hd_geometry.top_margin > 0);

    // Higher-density logical canvases scale the surface proportionally.
    assert(qhd_geometry.surface_width > hd_geometry.surface_width);
    assert(qhd_geometry.max_surface_height > hd_geometry.max_surface_height);

    // Ultrawides retain a readable short-edge-derived width instead of growing
    // in proportion to their enormous horizontal span.
    assert(ultrawide_geometry.surface_width == qhd_geometry.surface_width);

    // Portrait monitors use more of their scarce horizontal space, but never
    // exceed the monitor's edge-safe envelope.
    assert(portrait_geometry.surface_width > hd_geometry.surface_width);
    assert(portrait_geometry.surface_width < portrait.logical_width);

    assert(hd_geometry.details_max_height < hd_geometry.max_surface_height);
    assert(qhd_geometry.details_max_height < qhd_geometry.max_surface_height);
    return 0;
}
