#include "ui/NotesGeometry.hpp"

namespace realmheart::ui {

NotesLayout notes_layout_for_display_tier(core::DisplayTier display_tier) noexcept {
    switch (display_tier) {
    case core::DisplayTier::P1440:
        return NotesLayout{
            .display_tier = display_tier,
            .window_width = 800,
            .window_height = 1067,
            .text_margin_horizontal = 24,
            .text_margin_top = 19,
            .text_margin_bottom = 21,
        };
    case core::DisplayTier::P4K:
        return NotesLayout{
            .display_tier = display_tier,
            .window_width = 1200,
            .window_height = 1600,
            .text_margin_horizontal = 36,
            .text_margin_top = 28,
            .text_margin_bottom = 32,
        };
    case core::DisplayTier::P1080:
    default:
        return NotesLayout{};
    }
}

NotesLayout notes_layout_for_logical_geometry(int width, int height) noexcept {
    return notes_layout_for_display_tier(
        core::display_tier_for_logical_geometry(width, height)
    );
}

} // namespace realmheart::ui
