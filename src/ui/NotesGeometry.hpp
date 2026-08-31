#pragma once

#include "core/DisplayTier.hpp"

namespace realmheart::ui {

struct NotesLayout {
    core::DisplayTier display_tier = core::DisplayTier::P1080;
    int window_width = 600;
    int window_height = 800;
    int text_margin_horizontal = 18;
    int text_margin_top = 14;
    int text_margin_bottom = 16;

    [[nodiscard]] bool operator==(const NotesLayout&) const = default;
};

[[nodiscard]] NotesLayout notes_layout_for_display_tier(
    core::DisplayTier display_tier
) noexcept;

[[nodiscard]] NotesLayout notes_layout_for_logical_geometry(
    int width,
    int height
) noexcept;

} // namespace realmheart::ui
