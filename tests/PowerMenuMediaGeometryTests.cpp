#include "ui/powermenu/PowerMenuMediaGeometry.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "PowerMenuMediaGeometryTests: " << message << '\n';
    std::exit(1);
}

bool near(double left, double right, double epsilon = 0.75) {
    return std::abs(left - right) <= epsilon;
}

} // namespace

int main() {
    using realmheart::ui::powermenu::power_menu_cover_crop;
    using realmheart::ui::powermenu::power_menu_cover_placement;

    // The authored 16:9 scene remains pixel-aligned on a normal 16:9 output.
    {
        const auto placement = power_menu_cover_placement(
            1920, 1080, 1920, 1080, 0.5
        );
        const auto crop = power_menu_cover_crop(
            1920, 1080, 1920, 1080, 0.5
        );
        require(placement.width == 1920 && placement.height == 1080,
                "16:9 placement must not scale unnecessarily");
        require(near(placement.x, 0.0) && near(placement.y, 0.0),
                "16:9 placement must not shift");
        require(crop.x == 0 && crop.y == 0 &&
                crop.width == 1920 && crop.height == 1080,
                "16:9 ripple capture must stay uncropped");
    }

    // 3440x1440 is wider than the 16:9 source. Both the live GtkPicture and
    // transition capture must discard vertical content symmetrically. Keeping
    // the crop centred preserves both the upper faces and the lower controls.
    {
        constexpr double anchor = 0.5;
        const auto placement = power_menu_cover_placement(
            1920, 1080, 3440, 1440, anchor
        );
        const auto crop = power_menu_cover_crop(
            1920, 1080, 3440, 1440, anchor
        );
        require(placement.width == 3440 && placement.height > 1440,
                "ultrawide live media must cover without horizontal stretch");
        require(near(
                    placement.y,
                    -0.5 * static_cast<double>(placement.height - 1440)
                ),
                "ultrawide live crop must remain centred");
        require(crop.width == 1920 && crop.height < 1080,
                "ultrawide ripple source must be aspect-cropped before upload");
        require(std::abs(
                    crop.y - (1080 - crop.height) / 2
                ) <= 1,
                "ultrawide ripple crop must use the same centred anchor");
        require(near(
                    static_cast<double>(crop.width) / crop.height,
                    3440.0 / 1440.0,
                    0.01
                ),
                "ultrawide ripple crop must match viewport aspect");
    }

    // A 32:9 output is the pathological case that previously stretched the
    // opening texture. It must still crop, never distort.
    {
        const auto crop = power_menu_cover_crop(
            1920, 1080, 5120, 1440, 0.5
        );
        require(crop.width == 1920 && crop.height < 600,
                "super-ultrawide ripple must crop aggressively instead of stretch");
        require(std::abs(crop.y - (1080 - crop.height) / 2) <= 1,
                "super-ultrawide crop must remain vertically centred");
    }

    // Portrait/less-wide viewports crop the source horizontally and remain
    // centred because vertical anchoring is irrelevant in this branch.
    {
        const auto placement = power_menu_cover_placement(
            1920, 1080, 1080, 1920, 0.5
        );
        const auto crop = power_menu_cover_crop(
            1920, 1080, 1080, 1920, 0.5
        );
        require(placement.height == 1920 && placement.width > 1080,
                "portrait live media must cover vertically");
        require(placement.x < 0.0,
                "portrait live media must centre its horizontal crop");
        require(crop.height == 1080 && crop.width < 1920,
                "portrait ripple must crop horizontally");
        require(crop.x == (1920 - crop.width) / 2,
                "portrait ripple crop must remain centred");
    }

    std::cout << "Power menu media geometry tests passed\n";
    return 0;
}
