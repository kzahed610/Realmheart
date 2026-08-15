#include "relictombs/ManaCoresLayout.hpp"

#include <cmath>
#include <numbers>

namespace realmheart::relictombs {

ManaCoresLayout ManaCoresLayout::for_height(int physical_height) {
    ManaCoresLayout l;

    // Reference dimensions at 1080p
    constexpr int kRefHeight = 1080;
    constexpr int kRefWidth = 1920;

    const double scale = static_cast<double>(physical_height) / kRefHeight;
    const double canvas_width = kRefWidth * scale;
    const double canvas_height = physical_height;

    l.canvas_width = canvas_width;
    l.canvas_height = canvas_height;

    // Core positioned on LEFT side (~50px from left edge, near taskbar), vertically centred
    l.core_centre_x = 50.0 * scale;  // ~50px from left edge at 1080p
    l.core_centre_y = canvas_height / 2.0;

    // Core radius: ~140px at 1080p (scaled)
    l.core_radius = 140.0 * scale;
    // Shrunk radius during apply: ~50px at 1080p
    l.core_radius_shrunk = 50.0 * scale;

    // Radials parked to the right of core
    // Park position: core right edge + 15px gap + radial radius
    l.radial_radius = 28.0 * scale;
    l.radial_spacing = 10.0 * scale;
    l.radials_park_x = l.core_centre_x + l.core_radius + 15.0 * scale + l.radial_radius;
    l.radials_park_y = l.core_centre_y;

    // Fan-in arc radius: ~180px at 1080p
    l.fan_arc_radius = 180.0 * scale;
    // Fan from -45° to +45° (left side of core)
    l.fan_start_angle = -std::numbers::pi / 4.0;  // -45 degrees
    l.fan_end_angle = std::numbers::pi / 4.0;      // +45 degrees

    // Visual styling
    l.border_thickness = 2.5 * scale;
    l.glow_extent = 18.0 * scale;

    return l;
}

} // namespace realmheart::relictombs