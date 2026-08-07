#pragma once

#include <string_view>

namespace realmheart::ui::workspace::animation {

inline constexpr std::string_view kWorkspaceOverviewTransparentCss = R"CSS(
window.realmheart-workspace-overview-window,
window.realmheart-workspace-overview-window > contents,
window.realmheart-workspace-overview-window > overlay,
.realmheart-workspace-overview-stack,
realmheart-workspace-overview-canvas,
.realmheart-workspace-morph-gl-area {
    background-color: transparent;
    background-image: none;
    border-color: transparent;
    box-shadow: none;
}
)CSS";

// The fullscreen stage backdrop belongs only to the stable native overview and
// to the private exact-scene capture. While the morph timeline is moving, the
// toplevel must remain transparent outside the geometry-owned reveal clips.
[[nodiscard]] bool workspace_morph_draw_opaque_stage(
    bool interactive,
    bool force_native_capture
) noexcept;

// Once the shader has produced a frame, it owns the final strip at each realm
// frontier. Pulling the rectangular native clip slightly behind that strip is
// what lets the procedural edge read as an irregular materialization boundary
// instead of a glow drawn over a perfectly straight cutoff.
[[nodiscard]] double workspace_morph_native_reveal_width(
    double reveal_width,
    bool shader_frame_ready,
    bool exact_visible
) noexcept;

} // namespace realmheart::ui::workspace::animation
