#pragma once

#include <algorithm>
#include <cmath>

namespace realmheart::ui::workspace {

struct WorkspaceOverviewViewportTransform {
    double scale = 1.0;
    double content_width = 1920.0;
    double content_height = 1080.0;

    [[nodiscard]] double to_reference_x(double x) const noexcept {
        return x / scale;
    }

    [[nodiscard]] double to_reference_y(double y) const noexcept {
        return y / scale;
    }

    [[nodiscard]] double to_widget_x(double x) const noexcept {
        return x * scale;
    }

    [[nodiscard]] double to_widget_y(double y) const noexcept {
        return y * scale;
    }
};

[[nodiscard]] inline WorkspaceOverviewViewportTransform
workspace_overview_viewport_transform(
    int logical_width,
    int logical_height,
    double reference_width = 1920.0,
    double reference_height = 1080.0
) noexcept {
    if (logical_width <= 0 || logical_height <= 0 ||
        !std::isfinite(reference_width) || !std::isfinite(reference_height) ||
        reference_width <= 0.0 || reference_height <= 0.0) {
        return {};
    }

    // The authored overview is a 16:9 stage.  Use a single uniform scale so
    // ultrawide outputs gain horizontal breathing room instead of stretching
    // every realm, card and character.  Keep the stage top-left anchored so
    // its Aether Spine morph coordinates continue to line up with the bar.
    const double scale = std::min(
        static_cast<double>(logical_width) / reference_width,
        static_cast<double>(logical_height) / reference_height
    );
    if (!std::isfinite(scale) || scale <= 0.0) return {};

    return {
        .scale = scale,
        .content_width = reference_width * scale,
        .content_height = reference_height * scale,
    };
}

} // namespace realmheart::ui::workspace
