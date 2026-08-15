#pragma once

#include <cstddef>
#include <array>

namespace realmheart::relictombs {

struct ManaCoresLayout {
    // Canvas dimensions (physical pixels)
    double canvas_width = 0.0;
    double canvas_height = 0.0;

    // Core (white mana core) - centre of the selector
    double core_centre_x = 0.0;
    double core_centre_y = 0.0;
    double core_radius = 0.0;           // expanded radius (~140px @ 1080p)
    double core_radius_shrunk = 0.0;    // during apply (~50px)

    // Radials (silver, yellow, orange) - parked to the right of core
    double radials_park_x = 0.0;        // centre-of-stack x
    double radials_park_y = 0.0;        // centre y (same as core)
    double radial_radius = 0.0;         // ~28px @ 1080p
    double radial_spacing = 10.0;       // 10px centre-to-centre separation

    // Assembly fan-in positions (where radials arrive before detaching)
    double fan_arc_radius = 0.0;        // ~180px from core during assembly
    double fan_start_angle = 0.0;       // -45 degrees (left side)
    double fan_end_angle = 0.0;         // +45 degrees (right side)

    // Visual styling
    double border_thickness = 2.5;      // 2.5px
    double glow_extent = 18.0;          // 18px glow radius beyond border

    // Radial colour palette: {r, g, b} for silver, yellow, orange
    inline static const std::array<std::array<double, 3>, 3> kRadialPalette = {{
        {0.75, 0.75, 0.78},   // silver
        {0.95, 0.85, 0.30},   // yellow
        {0.98, 0.60, 0.20},   // orange
    }};

    // Factory for a given physical height (width scales proportionally)
    [[nodiscard]] static ManaCoresLayout for_height(int physical_height);
};

} // namespace realmheart::relictombs