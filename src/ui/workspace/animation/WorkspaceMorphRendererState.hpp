#pragma once

namespace realmheart::ui::workspace::animation {

[[nodiscard]] double workspace_morph_overlay_opacity(
    double progress,
    bool frame_ready
) noexcept;

// Pure lifecycle state for the transition-only GL enhancement layer. Keeping
// direction/progress/readiness outside GTK/GL makes reversal and cleanup
// behavior testable without a graphics context.
class WorkspaceMorphRendererState {
public:
    void begin(bool opening) noexcept;
    void update(double progress, bool opening) noexcept;
    void mark_frame_ready() noexcept;
    void finish() noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool frame_ready() const noexcept;
    [[nodiscard]] bool opening() const noexcept;
    [[nodiscard]] double progress() const noexcept;

private:
    bool active_ = false;
    bool frame_ready_ = false;
    bool opening_ = true;
    double progress_ = 0.0;
};

} // namespace realmheart::ui::workspace::animation
