#include "ui/powermenu/PowerMenuLayout.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using realmheart::ui::powermenu::PowerMenuAction;
using realmheart::ui::powermenu::PowerMenuLayout;

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void require_near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 0.01, message);
}

void test_action_order_and_labels_match_reference() {
    constexpr std::array expected{
        PowerMenuAction::Lock,
        PowerMenuAction::Suspend,
        PowerMenuAction::Logout,
        PowerMenuAction::Reboot,
        PowerMenuAction::PowerOff,
    };
    constexpr std::array labels{
        std::string_view{"LOCK"},
        std::string_view{"SUSPEND"},
        std::string_view{"LOG OUT"},
        std::string_view{"RESTART"},
        std::string_view{"SHUT DOWN"},
    };

    const auto buttons = realmheart::ui::powermenu::power_menu_buttons();
    require(buttons.size() == expected.size(), "reference must have five actions");
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        require(buttons[index].action == expected[index], "action order must match reference");
        require(buttons[index].label == labels[index], "action label must match reference");
    }
}

void test_design_geometry_scales_to_runtime_target() {
    const PowerMenuLayout layout = realmheart::ui::powermenu::power_menu_layout(1920.0, 1080.0);

    require_near(layout.scale, 0.8, "2400x1350 base must scale uniformly to 1920x1080");
    require_near(layout.offset_x, 0.0, "16:9 target must not shift base horizontally");
    require_near(layout.offset_y, 0.0, "16:9 target must not shift base vertically");

    const auto& lock = layout.buttons[0].bounds;
    require_near(lock.x, 744.0, "lock button x must match the measured reference crop");
    require_near(lock.y, 335.2, "lock button y must match the approved design geometry");
    require_near(lock.width, 432.0, "lock button width must match the measured reference crop");
    require_near(lock.height, 68.8, "lock button height must match the approved design geometry");

    const auto& power = layout.buttons[4].bounds;
    require_near(power.x, 744.0, "shutdown must share the normal button x position");
    require_near(power.y, 784.0, "shutdown must be compact until it is hovered");
    require_near(power.width, 432.0, "shutdown must share the normal button width");
    require_near(power.height, 68.8, "shutdown must share the normal button height");
}

void test_all_actions_share_one_normal_size() {
    const auto& buttons = realmheart::ui::powermenu::power_menu_buttons();
    const auto& normal = buttons.front().design_bounds;
    for (const auto& button : buttons) {
        require_near(
            button.design_bounds.width,
            normal.width,
            "every resting action must use the same width"
        );
        require_near(
            button.design_bounds.height,
            normal.height,
            "every resting action must use the same height"
        );
    }
}

void test_hover_state_expands_around_each_button_center() {
    const PowerMenuLayout layout = realmheart::ui::powermenu::power_menu_layout(1920.0, 1080.0);
    for (const auto& button : layout.buttons) {
        const auto& normal = button.bounds;
        const auto& hover = button.hover_bounds;
        require_near(
            hover.x + hover.width * 0.5,
            normal.x + normal.width * 0.5,
            "hover width must expand around the normal center"
        );
        require_near(
            hover.y + hover.height * 0.5,
            normal.y + normal.height * 0.5,
            "hover height must expand around the normal center"
        );
        require_near(hover.width, 472.0, "hover width must match the measured reference state");
        require_near(hover.height, 80.0, "hover height must match the approved expanded geometry");
    }

    const auto& shutdown = layout.buttons.back();
    require_near(shutdown.hover_bounds.x, 724.0, "shutdown hover must remain exactly centered");
    require_near(shutdown.hover_bounds.y, 778.4, "shutdown hover y must remain centered");
}

void test_visual_state_selects_hover_bounds_only_while_expanded() {
    const PowerMenuLayout layout = realmheart::ui::powermenu::power_menu_layout(1920.0, 1080.0);
    const auto& shutdown = layout.buttons.back();
    const auto& resting = realmheart::ui::powermenu::power_menu_button_bounds(shutdown, false);
    const auto& hovered = realmheart::ui::powermenu::power_menu_button_bounds(shutdown, true);

    require_near(resting.x, shutdown.bounds.x, "resting state must select compact bounds");
    require_near(resting.width, shutdown.bounds.width, "resting state must remain compact");
    require_near(hovered.x, shutdown.hover_bounds.x, "hover state must select expanded bounds");
    require_near(hovered.width, shutdown.hover_bounds.width, "hover state must expand");
}

void test_ultrawide_controls_fit_height_and_remain_fully_visible() {
    const PowerMenuLayout layout = realmheart::ui::powermenu::power_menu_layout(2560.0, 1080.0);

    require_near(layout.scale, 0.8, "1080p ultrawide controls must retain 1080p density");
    require_near(layout.offset_x, 320.0, "extra ultrawide width must be split around the controls");
    require_near(layout.offset_y, 0.0, "height-fit controls must not be vertically cropped");
    require_near(
        layout.buttons[0].bounds.x,
        320.0 + 930.0 * 0.8,
        "ultrawide controls must remain centered in the viewport"
    );

    const auto& power = layout.buttons.back().hover_bounds;
    require(
        power.y >= 0.0 && power.y + power.height <= 1080.0,
        "ultrawide shutdown hover state must remain fully on-screen"
    );
}

void test_super_ultrawide_controls_do_not_follow_media_cover_scale() {
    const PowerMenuLayout layout = realmheart::ui::powermenu::power_menu_layout(5120.0, 1440.0);

    const double expected_scale = 1440.0 / 1350.0;
    require_near(
        layout.scale,
        expected_scale,
        "32:9 controls must follow output height rather than media cover width"
    );
    require_near(
        layout.offset_x,
        (5120.0 - 2400.0 * expected_scale) * 0.5,
        "32:9 controls must center the fitted authored canvas horizontally"
    );
    const auto& first_hover = layout.buttons.front().hover_bounds;
    const auto& last_hover = layout.buttons.back().hover_bounds;
    const double top_margin = first_hover.y;
    const double bottom_margin = 1440.0 - (last_hover.y + last_hover.height);
    require_near(
        top_margin,
        bottom_margin,
        "32:9 must center the full hover-expanded action stack vertically"
    );
    require(
        top_margin >= 1440.0 * 0.04,
        "32:9 action stack must retain the viewport-safe top margin"
    );

    for (const auto& button : layout.buttons) {
        for (const auto* bounds : {&button.bounds, &button.hover_bounds}) {
            require(bounds->x >= 0.0, "32:9 button must not clip the left edge");
            require(bounds->y >= 0.0, "32:9 button must not clip the top edge");
            require(bounds->x + bounds->width <= 5120.0, "32:9 button must not clip the right edge");
            require(bounds->y + bounds->height <= 1440.0, "32:9 button must not clip the bottom edge");
        }
    }
}

} // namespace

int main() {
    test_action_order_and_labels_match_reference();
    test_design_geometry_scales_to_runtime_target();
    test_all_actions_share_one_normal_size();
    test_hover_state_expands_around_each_button_center();
    test_visual_state_selects_hover_bounds_only_while_expanded();
    test_ultrawide_controls_fit_height_and_remain_fully_visible();
    test_super_ultrawide_controls_do_not_follow_media_cover_scale();
    std::cout << "PowerMenuLayout tests passed\n";
    return 0;
}
