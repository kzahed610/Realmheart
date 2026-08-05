#pragma once

#include <gtk/gtk.h>

#include <string>

namespace realmheart::ui::powermenu::animation {

// Transition-only renderer for the full-screen power-menu scene. It snapshots
// the current video/poster frame once, then reveals that frame through a
// transparent GtkGLArea. The normal GtkVideo takes over again when the ripple
// reaches its terminal frame, so no transition textures remain resident while
// the menu is idle.
class PowerMenuRippleRenderer {
public:
    PowerMenuRippleRenderer();
    ~PowerMenuRippleRenderer();

    PowerMenuRippleRenderer(const PowerMenuRippleRenderer&) = delete;
    PowerMenuRippleRenderer& operator=(const PowerMenuRippleRenderer&) = delete;

    [[nodiscard]] GtkWidget* widget() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool frame_ready() const noexcept;

    [[nodiscard]] bool begin(
        GdkPaintable* source,
        double normalized_origin_x,
        double normalized_origin_y,
        bool opening,
        std::string* error = nullptr
    );

    // Replace the transition texture with the paintable's current frame while
    // preserving the active ripple timeline. Opening uses this to reveal the
    // actually playing video instead of a frozen first-frame poster.
    [[nodiscard]] bool refresh_source(
        GdkPaintable* source,
        std::string* error = nullptr
    );

    void update(double progress, bool opening) noexcept;
    void set_opacity(double opacity) noexcept;
    void finish() noexcept;

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::ui::powermenu::animation
