#pragma once

#include <algorithm>
#include <cmath>

namespace realmheart::ui::powermenu {

struct PowerMenuCoverPlacement {
    int width = 1;
    int height = 1;
    double x = 0.0;
    double y = 0.0;
};

struct PowerMenuCoverCrop {
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

[[nodiscard]] inline PowerMenuCoverPlacement power_menu_cover_placement(
    int source_width,
    int source_height,
    int viewport_width,
    int viewport_height,
    double vertical_anchor
) noexcept {
    source_width = std::max(source_width, 1);
    source_height = std::max(source_height, 1);
    viewport_width = std::max(viewport_width, 1);
    viewport_height = std::max(viewport_height, 1);
    vertical_anchor = std::clamp(vertical_anchor, 0.0, 1.0);

    const double source_aspect = static_cast<double>(source_width) /
        static_cast<double>(source_height);
    const double viewport_aspect = static_cast<double>(viewport_width) /
        static_cast<double>(viewport_height);

    PowerMenuCoverPlacement result{
        viewport_width,
        viewport_height,
        0.0,
        0.0
    };
    if (source_aspect < viewport_aspect) {
        result.height = std::max(
            viewport_height,
            static_cast<int>(std::lround(
                static_cast<double>(viewport_width) / source_aspect
            ))
        );
        result.y = -static_cast<double>(result.height - viewport_height) *
            vertical_anchor;
    } else if (source_aspect > viewport_aspect) {
        result.width = std::max(
            viewport_width,
            static_cast<int>(std::lround(
                static_cast<double>(viewport_height) * source_aspect
            ))
        );
        result.x = -0.5 * static_cast<double>(result.width - viewport_width);
    }
    return result;
}

[[nodiscard]] inline PowerMenuCoverCrop power_menu_cover_crop(
    int source_width,
    int source_height,
    int viewport_width,
    int viewport_height,
    double vertical_anchor
) noexcept {
    source_width = std::max(source_width, 1);
    source_height = std::max(source_height, 1);
    viewport_width = std::max(viewport_width, 1);
    viewport_height = std::max(viewport_height, 1);
    vertical_anchor = std::clamp(vertical_anchor, 0.0, 1.0);

    const double source_aspect = static_cast<double>(source_width) /
        static_cast<double>(source_height);
    const double viewport_aspect = static_cast<double>(viewport_width) /
        static_cast<double>(viewport_height);

    PowerMenuCoverCrop result{0, 0, source_width, source_height};
    if (source_aspect < viewport_aspect) {
        result.height = std::clamp(
            static_cast<int>(std::lround(
                static_cast<double>(source_width) / viewport_aspect
            )),
            1,
            source_height
        );
        result.y = std::clamp(
            static_cast<int>(std::lround(
                static_cast<double>(source_height - result.height) *
                vertical_anchor
            )),
            0,
            source_height - result.height
        );
    } else if (source_aspect > viewport_aspect) {
        result.width = std::clamp(
            static_cast<int>(std::lround(
                static_cast<double>(source_height) * viewport_aspect
            )),
            1,
            source_width
        );
        result.x = (source_width - result.width) / 2;
    }
    return result;
}

} // namespace realmheart::ui::powermenu
