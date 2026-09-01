#include "ui/powermenu/PowerMenuLayout.hpp"

#include <algorithm>

namespace realmheart::ui::powermenu {
namespace {

constexpr double kDesignWidth = 2400.0;
constexpr double kDesignHeight = 1350.0;
constexpr double kHoverWidth = 590.0;
constexpr double kHoverHeight = 100.0;
constexpr double kSuperUltrawideAspectThreshold = 3.0;

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

    // Interactive controls must always remain fully inside the output. The
    // background/video intentionally uses a cover crop, but reusing that
    // transform for controls causes 21:9/32:9 outputs to scale the authored
    // 16:9 button stack by width and push the lower actions off-screen.
    //
    // Fit the authored control canvas inside the viewport instead. On 16:9
    // outputs min == max, so the approved baseline is unchanged. On ultrawide
    // outputs this makes control density follow the monitor height while the
    // extra horizontal space is simply split around the centered canvas.
    layout.scale = std::min(surface_width / kDesignWidth, surface_height / kDesignHeight);
    layout.offset_x = (surface_width - kDesignWidth * layout.scale) * 0.5;
    layout.offset_y = (surface_height - kDesignHeight * layout.scale) * 0.5;

    // On 32:9-class outputs, centering the entire authored 2400x1350 canvas
    // still leaves the actual action stack visually bottom-heavy because the
    // five buttons occupy only the lower-middle portion of that canvas. Center
    // the complete hover-expanded stack itself instead. This keeps REST/hover
    // states symmetric and makes SHUT DOWN as safe as LOCK without touching the
    // approved 16:9 or ordinary 21:9 composition.
    const double aspect = surface_width / surface_height;
    if (aspect >= kSuperUltrawideAspectThreshold) {
        const PowerMenuRect first_hover = hover_bounds(kButtons.front().design_bounds);
        const PowerMenuRect last_hover = hover_bounds(kButtons.back().design_bounds);
        const double stack_top = first_hover.y;
        const double stack_bottom = last_hover.y + last_hover.height;
        const double stack_mid = (stack_top + stack_bottom) * 0.5;

        layout.offset_y = surface_height * 0.5 - stack_mid * layout.scale;

        // Keep a modest viewport-safe margin even if a future authored button
        // moves farther up/down. The stack easily fits at the supported 32:9
        // densities, so this clamp is a guardrail rather than a scale change.
        const double safe_margin = std::max(24.0, surface_height * 0.04);
        const double min_offset = safe_margin - stack_top * layout.scale;
        const double max_offset =
            surface_height - safe_margin - stack_bottom * layout.scale;
        if (min_offset <= max_offset) {
            layout.offset_y = std::clamp(layout.offset_y, min_offset, max_offset);
        }
    }

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
