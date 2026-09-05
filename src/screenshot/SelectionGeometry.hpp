#pragma once

#include <algorithm>
#include <cmath>

namespace realmheart::screenshot {

enum class SelectionRatio {
    Free = 0,
    Ratio16x9,
    Square,
    Ratio4x3,
};

struct SelectionRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct PixelRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

inline bool selection_ratio_is_fixed(SelectionRatio ratio) {
    return ratio != SelectionRatio::Free;
}

inline double selection_ratio_value(SelectionRatio ratio) {
    switch (ratio) {
        case SelectionRatio::Ratio16x9:
            return 16.0 / 9.0;
        case SelectionRatio::Square:
            return 1.0;
        case SelectionRatio::Ratio4x3:
            return 4.0 / 3.0;
        case SelectionRatio::Free:
        default:
            return 0.0;
    }
}

inline const char* selection_ratio_label(SelectionRatio ratio) {
    switch (ratio) {
        case SelectionRatio::Ratio16x9:
            return "16:9";
        case SelectionRatio::Square:
            return "1:1";
        case SelectionRatio::Ratio4x3:
            return "4:3";
        case SelectionRatio::Free:
        default:
            return "Free";
    }
}

inline bool selection_coordinates_finite(
    double start_x,
    double start_y,
    double current_x,
    double current_y
) {
    return std::isfinite(start_x) && std::isfinite(start_y) &&
        std::isfinite(current_x) && std::isfinite(current_y);
}

inline SelectionRect normalize_selection(
    double start_x,
    double start_y,
    double current_x,
    double current_y
) {
    if (!selection_coordinates_finite(start_x, start_y, current_x, current_y)) {
        return {};
    }
    const double left = std::min(start_x, current_x);
    const double top = std::min(start_y, current_y);
    const double right = std::max(start_x, current_x);
    const double bottom = std::max(start_y, current_y);

    return SelectionRect{
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };
}

inline int selection_direction(double delta, double anchor, double extent) {
    constexpr double epsilon = 0.0001;
    if (delta < -epsilon) return -1;
    if (delta > epsilon) return 1;

    // If the pointer has not moved on this axis yet, grow toward whichever
    // side has more room. This keeps vertical-only/horizontal-only drags usable
    // even when the anchor starts directly against a monitor edge.
    return anchor > extent / 2.0 ? -1 : 1;
}

inline SelectionRect selection_for_ratio(
    double start_x,
    double start_y,
    double current_x,
    double current_y,
    int logical_width,
    int logical_height,
    SelectionRatio ratio
) {
    const double max_x = static_cast<double>(std::max(0, logical_width));
    const double max_y = static_cast<double>(std::max(0, logical_height));
    if (!selection_coordinates_finite(start_x, start_y, current_x, current_y)) {
        return {};
    }

    start_x = std::clamp(start_x, 0.0, max_x);
    start_y = std::clamp(start_y, 0.0, max_y);
    current_x = std::clamp(current_x, 0.0, max_x);
    current_y = std::clamp(current_y, 0.0, max_y);

    if (!selection_ratio_is_fixed(ratio)) {
        return normalize_selection(start_x, start_y, current_x, current_y);
    }

    const double target_ratio = selection_ratio_value(ratio);
    if (target_ratio <= 0.0) return {};

    const double delta_x = current_x - start_x;
    const double delta_y = current_y - start_y;
    const double abs_x = std::abs(delta_x);
    const double abs_y = std::abs(delta_y);

    double width = 0.0;
    double height = 0.0;

    // Whichever physical pointer axis moved farther drives the selection.
    // The opposite axis follows the requested aspect ratio.
    if (abs_x >= abs_y) {
        width = abs_x;
        height = width / target_ratio;
    } else {
        height = abs_y;
        width = height * target_ratio;
    }

    const int direction_x = selection_direction(delta_x, start_x, max_x);
    const int direction_y = selection_direction(delta_y, start_y, max_y);
    const double available_width = direction_x > 0 ? max_x - start_x : start_x;
    const double available_height = direction_y > 0 ? max_y - start_y : start_y;

    if (width > 0.0 && height > 0.0) {
        const double width_scale = available_width / width;
        const double height_scale = available_height / height;
        const double scale = std::clamp(
            std::min({1.0, width_scale, height_scale}),
            0.0,
            1.0
        );
        width *= scale;
        height *= scale;
    }

    return SelectionRect{
        .x = direction_x > 0 ? start_x : start_x - width,
        .y = direction_y > 0 ? start_y : start_y - height,
        .width = width,
        .height = height,
    };
}

inline PixelRect selection_to_pixels(
    const SelectionRect& selection,
    int logical_width,
    int logical_height,
    int frame_width,
    int frame_height
) {
    if (
        logical_width <= 0 || logical_height <= 0 ||
        frame_width <= 0 || frame_height <= 0 ||
        !std::isfinite(selection.x) || !std::isfinite(selection.y) ||
        !std::isfinite(selection.width) || !std::isfinite(selection.height) ||
        selection.width <= 0.0 || selection.height <= 0.0
    ) {
        return {};
    }

    const double scale_x = static_cast<double>(frame_width) /
        static_cast<double>(logical_width);
    const double scale_y = static_cast<double>(frame_height) /
        static_cast<double>(logical_height);

    const int left = std::clamp(
        static_cast<int>(std::floor(selection.x * scale_x)),
        0,
        frame_width
    );
    const int top = std::clamp(
        static_cast<int>(std::floor(selection.y * scale_y)),
        0,
        frame_height
    );
    const int right = std::clamp(
        static_cast<int>(std::ceil((selection.x + selection.width) * scale_x)),
        0,
        frame_width
    );
    const int bottom = std::clamp(
        static_cast<int>(std::ceil((selection.y + selection.height) * scale_y)),
        0,
        frame_height
    );

    return PixelRect{
        .x = left,
        .y = top,
        .width = std::max(0, right - left),
        .height = std::max(0, bottom - top),
    };
}

} // namespace realmheart::screenshot
