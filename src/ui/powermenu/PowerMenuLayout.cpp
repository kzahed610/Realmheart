#include "ui/powermenu/PowerMenuLayout.hpp"

#include <algorithm>

namespace realmheart::ui::powermenu {
namespace {

constexpr double kDesignWidth = 2400.0;
constexpr double kDesignHeight = 1350.0;
constexpr double kHoverWidth = 590.0;
constexpr double kHoverHeight = 100.0;

constexpr std::array<PowerMenuButtonDefinition, 5> kButtons{{
    {PowerMenuAction::Lock, "LOCK", {930.0, 419.0, 540.0, 86.0}},
    {PowerMenuAction::Suspend, "SUSPEND", {930.0, 558.0, 540.0, 86.0}},
    {PowerMenuAction::Logout, "LOG OUT", {930.0, 697.0, 540.0, 86.0}},
    {PowerMenuAction::Reboot, "RESTART", {930.0, 836.0, 540.0, 86.0}},
    {PowerMenuAction::PowerOff, "SHUT DOWN", {930.0, 980.0, 540.0, 86.0}},
}};

PowerMenuRect transform_rect(
    const PowerMenuRect& rect,
    double scale,
    double offset_x,
    double offset_y
) {
    return {
        offset_x + rect.x * scale,
        offset_y + rect.y * scale,
        rect.width * scale,
        rect.height * scale,
    };
}

PowerMenuRect hover_bounds(const PowerMenuRect& normal) {
    return {
        normal.x + (normal.width - kHoverWidth) * 0.5,
        normal.y + (normal.height - kHoverHeight) * 0.5,
        kHoverWidth,
        kHoverHeight,
    };
}

} // namespace

const std::array<PowerMenuButtonDefinition, 5>& power_menu_buttons() {
    return kButtons;
}

PowerMenuLayout power_menu_layout(double surface_width, double surface_height) {
    PowerMenuLayout layout;
    if (surface_width <= 0.0 || surface_height <= 0.0) return layout;

    layout.scale = std::max(surface_width / kDesignWidth, surface_height / kDesignHeight);
    layout.offset_x = (surface_width - kDesignWidth * layout.scale) * 0.5;
    layout.offset_y = (surface_height - kDesignHeight * layout.scale) * 0.5;

    for (std::size_t index = 0; index < kButtons.size(); ++index) {
        const auto& definition = kButtons[index];
        layout.buttons[index] = {
            definition.action,
            definition.label,
            transform_rect(
                definition.design_bounds,
                layout.scale,
                layout.offset_x,
                layout.offset_y
            ),
            transform_rect(
                hover_bounds(definition.design_bounds),
                layout.scale,
                layout.offset_x,
                layout.offset_y
            ),
        };
    }
    return layout;
}

const PowerMenuRect& power_menu_button_bounds(
    const PowerMenuButtonLayout& button,
    bool expanded
) {
    return expanded ? button.hover_bounds : button.bounds;
}

} // namespace realmheart::ui::powermenu
