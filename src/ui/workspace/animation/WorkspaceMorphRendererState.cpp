#include "ui/workspace/animation/WorkspaceMorphRendererState.hpp"

#include <algorithm>

namespace realmheart::ui::workspace::animation {
namespace {

[[nodiscard]] double smoothstep01(double value) noexcept {
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

} // namespace

double workspace_morph_overlay_opacity(
    double progress,
    bool frame_ready
) noexcept {
    // GtkGLArea documents its initial framebuffer as transparent. Keep the
    // widget at full compositing opacity until the first render instead of
    // relying on a near-zero opacity node that renderers may optimize away.
    if (!frame_ready) return 1.0;
    constexpr double kHandoffWidth = 0.035;
    const double clamped = std::clamp(progress, 0.0, 1.0);
    const double from_hidden = smoothstep01(clamped / kHandoffWidth);
    const double from_visible = smoothstep01(
        (1.0 - clamped) / kHandoffWidth
    );
    return std::min(from_hidden, from_visible);
}

void WorkspaceMorphRendererState::begin(bool opening) noexcept {
    active_ = true;
    frame_ready_ = false;
    opening_ = opening;
    progress_ = opening ? 0.0 : 1.0;
}

void WorkspaceMorphRendererState::update(
    double progress,
    bool opening
) noexcept {
    if (!active_) return;
    progress_ = std::clamp(progress, 0.0, 1.0);
    opening_ = opening;
}

void WorkspaceMorphRendererState::mark_frame_ready() noexcept {
    if (active_) frame_ready_ = true;
}

void WorkspaceMorphRendererState::finish() noexcept {
    active_ = false;
    frame_ready_ = false;
}

bool WorkspaceMorphRendererState::active() const noexcept {
    return active_;
}

bool WorkspaceMorphRendererState::frame_ready() const noexcept {
    return active_ && frame_ready_;
}

bool WorkspaceMorphRendererState::opening() const noexcept {
    return opening_;
}

double WorkspaceMorphRendererState::progress() const noexcept {
    return progress_;
}

} // namespace realmheart::ui::workspace::animation
