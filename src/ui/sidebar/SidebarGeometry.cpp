#include "ui/sidebar/SidebarGeometry.hpp"

namespace realmheart::ui::sidebar {

SidebarFrameLayout SidebarFrameLayout::for_display_tier(
    core::DisplayTier display_tier
) noexcept {
    const double scale = core::display_tier_spec(display_tier).scale;
    SidebarFrameLayout layout;
    layout.frame_width = core::scale_dimension(486, scale);
    layout.character_gutter_width = core::scale_dimension(240, scale);
    layout.content_inset_start = core::scale_dimension(76, scale);
    layout.content_inset_end = core::scale_dimension(42, scale);
    layout.content_inset_top = core::scale_dimension(38, scale);
    layout.content_inset_bottom = core::scale_dimension(38, scale);
    layout.right_margin = core::scale_dimension(2, scale);
    return layout;
}

} // namespace realmheart::ui::sidebar
