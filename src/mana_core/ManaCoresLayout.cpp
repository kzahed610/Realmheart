#include "mana_core/ManaCoresLayout.hpp"

#include <cmath>
#include <numbers>

namespace realmheart::mana_core {

ManaCoresLayout ManaCoresLayout::for_height(int physical_height, int physical_width) {
    ManaCoresLayout l;

    // Reference dimensions at 1080p
    constexpr int kRefHeight = 1080;
    constexpr int kRefWidth = 1920;

    const double scale = static_cast<double>(physical_height) / kRefHeight;
    const double canvas_height = physical_height;
    const double canvas_width = (physical_width > 0) ? static_cast<double>(physical_width) : (kRefWidth * scale);

    l.canvas_width = canvas_width;
    l.canvas_height = canvas_height;

    // Core is centered vertically. Horizontally it is moved to the left side
    // of the screen, ~50px away from a standard left taskbar (assuming ~50px taskbar width)
    // so it sits at around x=200px (with its radius of 220px, it fits beautifully).
    l.core_centre_x = 320.0 * scale;
    l.core_centre_y = canvas_height / 2.0;

    // Core radii
    l.core_radius_expanded = 220.0 * scale;  // Large enough to comfortably preview wallpaper
    l.core_radius_small = 45.0 * scale;      // Compact core during emerge & assembly
    l.core_radius_shrunk = 35.0 * scale;     // Shrunk core during apply & exit

    // Slices (annular sectors)
    l.slice_gap = 40.0 * scale;              // Gap between core border and slice inner arc (increased from 18)
    l.slice_depth_expanded = 120.0 * scale;  // Radial depth of slices when expanded (increased from 95)
    l.slice_depth_attached = 35.0 * scale;   // Radial depth of slices when attached to small core

    // Detached state: 3 slices parked on the right side of the core
    // 8-degree gaps, 36-degree span each
    constexpr double deg2rad = std::numbers::pi / 180.0;

    l.detached_slices[0].start_angle = -60.0 * deg2rad;
    l.detached_slices[0].end_angle = -24.0 * deg2rad;
    l.detached_slices[0].mid_angle = -42.0 * deg2rad;

    l.detached_slices[1].start_angle = -16.0 * deg2rad;
    l.detached_slices[1].end_angle = 16.0 * deg2rad;
    l.detached_slices[1].mid_angle = 0.0;

    l.detached_slices[2].start_angle = 24.0 * deg2rad;
    l.detached_slices[2].end_angle = 60.0 * deg2rad;
    l.detached_slices[2].mid_angle = 42.0 * deg2rad;

    // Attached state: 3 slices fully encompassing the core forming a giant circle.
    // Each takes up exactly 120 degrees
    l.attached_slices[0].start_angle = -180.0 * deg2rad;
    l.attached_slices[0].end_angle = -60.0 * deg2rad;
    l.attached_slices[0].mid_angle = -120.0 * deg2rad;

    l.attached_slices[1].start_angle = -60.0 * deg2rad;
    l.attached_slices[1].end_angle = 60.0 * deg2rad;
    l.attached_slices[1].mid_angle = 0.0;

    l.attached_slices[2].start_angle = 60.0 * deg2rad;
    l.attached_slices[2].end_angle = 180.0 * deg2rad;
    l.attached_slices[2].mid_angle = 120.0 * deg2rad;

    // Visual styling
    l.border_thickness = 2.5 * scale;
    l.glow_extent = 16.0 * scale;
    l.star_spike_length = 16.0 * scale;

    return l;
}

} // namespace realmheart::mana_core