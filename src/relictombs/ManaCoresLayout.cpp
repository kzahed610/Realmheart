#include "relictombs/ManaCoresLayout.hpp"

#include <cmath>
#include <numbers>

namespace realmheart::relictombs {

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
    l.slice_gap = 18.0 * scale;              // Gap between core border and slice inner arc (~15-20px)
    l.slice_depth_expanded = 95.0 * scale;   // Radial depth of slices when expanded
    l.slice_depth_attached = 35.0 * scale;   // Radial depth of slices when attached to small core

    // Detached state: 3 slices parked on the right side of the core
    // Top (Silver): -52° to -18°
    // Middle (Yellow): -14° to +14°
    // Bottom (Orange): +18° to +52°
    // (with 4° angular gaps between slices)
    constexpr double deg2rad = std::numbers::pi / 180.0;

    l.detached_slices[0].start_angle = -52.0 * deg2rad;
    l.detached_slices[0].end_angle = -18.0 * deg2rad;
    l.detached_slices[0].mid_angle = (-52.0 + -18.0) * 0.5 * deg2rad;

    l.detached_slices[1].start_angle = -14.0 * deg2rad;
    l.detached_slices[1].end_angle = 14.0 * deg2rad;
    l.detached_slices[1].mid_angle = 0.0;

    l.detached_slices[2].start_angle = 18.0 * deg2rad;
    l.detached_slices[2].end_angle = 52.0 * deg2rad;
    l.detached_slices[2].mid_angle = (18.0 + 52.0) * 0.5 * deg2rad;

    // Attached state: 3 slices fully encompassing the core forming a giant circle.
    // Each takes up exactly 120 degrees (-90 to +30, +30 to +150, +150 to +270/-90)
    l.attached_slices[0].start_angle = -90.0 * deg2rad;
    l.attached_slices[0].end_angle = 30.0 * deg2rad;
    l.attached_slices[0].mid_angle = -30.0 * deg2rad;

    l.attached_slices[1].start_angle = -30.0 * deg2rad; // Wait, actually the order is top, middle, bottom. Let's arrange them so interpolation looks best!
    // Since detached are: 0=top, 1=mid, 2=bot, we should map them appropriately.
    // Top parked is around -35 deg. Middle is 0. Bottom is +35 deg.
    // Let's divide 360 by 3:
    // Slice 1 (Yellow, Middle): -60 to +60 (centered at 0)
    // Slice 2 (Orange, Bottom): +60 to +180 (centered at 120)
    // Slice 0 (Silver, Top): +180 to +300 / -180 to -60 (centered at -120)

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

} // namespace realmheart::relictombs