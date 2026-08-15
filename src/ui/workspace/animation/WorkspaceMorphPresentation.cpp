#include "ui/workspace/animation/WorkspaceMorphPresentation.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::workspace::animation {

bool workspace_morph_draw_opaque_stage(
    bool interactive,
    bool force_native_capture
) noexcept {
    return interactive || force_native_capture;
}

double workspace_morph_native_reveal_width(
    double reveal_width,
    bool shader_frame_ready,
    bool exact_visible
) noexcept {
    if (!std::isfinite(reveal_width)) return 0.0;
    const double safe_width = std::max(0.0, reveal_width);
    if (!shader_frame_ready || exact_visible) return safe_width;
    constexpr double kShaderOwnedFrontierWidth = 52.0;
    return std::max(0.0, safe_width - kShaderOwnedFrontierWidth);
}

} // namespace realmheart::ui::workspace::animation
