#pragma once

#include "ui/powermenu/PowerMenuConfirmation.hpp"

#include <array>
#include <string_view>

namespace realmheart::ui::powermenu {

using PowerMenuAction = PowerMenuConfirmation::Action;

struct PowerMenuRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct PowerMenuButtonDefinition {
    PowerMenuAction action;
    std::string_view label;
    PowerMenuRect design_bounds;
};

struct PowerMenuButtonLayout {
    PowerMenuAction action;
    std::string_view label;
    PowerMenuRect bounds;
    PowerMenuRect hover_bounds;
};

struct PowerMenuLayout {
    double scale = 0.0;
    double offset_x = 0.0;
    double offset_y = 0.0;
    std::array<PowerMenuButtonLayout, 5> buttons{};
};

[[nodiscard]] const std::array<PowerMenuButtonDefinition, 5>& power_menu_buttons();
[[nodiscard]] PowerMenuLayout power_menu_layout(double surface_width, double surface_height);
[[nodiscard]] const PowerMenuRect& power_menu_button_bounds(
    const PowerMenuButtonLayout& button,
    bool expanded
);

} // namespace realmheart::ui::powermenu
