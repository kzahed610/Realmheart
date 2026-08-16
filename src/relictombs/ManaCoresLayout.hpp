#pragma once

#include <cstddef>
#include <array>

namespace realmheart::relictombs {

struct RadialSliceGeometry {
    double start_angle = 0.0;
    double end_angle = 0.0;
    double mid_angle = 0.0;
};

struct ManaCoresLayout {
    // Canvas dimensions (physical pixels)
    double canvas_width = 0.0;
    double canvas_height = 0.0;

    // Core (white mana core) - centre of the selector
    double core_centre_x = 0.0;
    double core_centre_y = 0.0;
    double core_radius_expanded = 0.0;  // ~220px @ 1080p
    double core_radius_small = 0.0;     // ~45px @ 1080p
    double core_radius_shrunk = 0.0;    // ~35px @ 1080p during apply

    // Radial slices (annular sectors: Silver, Yellow, Orange)
    double slice_gap = 0.0;             // ~18px gap between core and slices
    double slice_depth_expanded = 0.0;  // ~95px radial width when expanded
    double slice_depth_attached = 0.0;  // ~35px radial width when attached

    // Angular geometry of the 3 slices fully encompassing the core (Attached state)
    std::array<RadialSliceGeometry, 3> attached_slices;

    // Angular geometry of the 3 slices parked on the right perimeter (Detached state)
    std::array<RadialSliceGeometry, 3> detached_slices;

    // Visual styling
    double border_thickness = 2.5;      // 2.5px
    double glow_extent = 16.0;          // 16px glow radius beyond border
    double star_spike_length = 16.0;    // 16px cardinal star ornaments

    // Radial colour palette: {r, g, b} for silver, yellow, orange
    inline static const std::array<std::array<double, 3>, 3> kRadialPalette = {{
        {0.78, 0.84, 0.92},   // silver / pale mana silver
        {0.96, 0.82, 0.24},   // yellow / bright gold
        {0.98, 0.52, 0.16},   // orange / radiant orange
    }};

    // Factory for a given physical height & optional width
    [[nodiscard]] static ManaCoresLayout for_height(int physical_height, int physical_width = 0);
};

} // namespace realmheart::relictombs