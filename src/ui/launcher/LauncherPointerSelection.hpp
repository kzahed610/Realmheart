#pragma once

namespace realmheart::ui::launcher {

[[nodiscard]] constexpr bool should_select_result_row_for_pointer_motion(
    bool special_picker,
    bool previous_position_valid,
    double previous_x,
    double previous_y,
    double current_x,
    double current_y,
    double epsilon
) noexcept {
    if (!previous_position_valid) return !special_picker;

    const double delta_x = current_x - previous_x;
    const double delta_y = current_y - previous_y;
    return delta_x > epsilon || delta_x < -epsilon ||
        delta_y > epsilon || delta_y < -epsilon;
}

} // namespace realmheart::ui::launcher
