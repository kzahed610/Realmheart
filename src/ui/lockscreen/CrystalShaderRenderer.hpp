#pragma once

#include <gtk/gtk.h>

#include <memory>

namespace realmheart::ui::lockscreen {

class ShaderManager;

// Per-frame scene values for the Broken Seal crystal shard. uSplit is 0 for
// the intact shard, 1 for the split halves; uAngle is the half-rotation
// (radians, 0..pi); uOffsetX is the horizontal half-drift (0..~0.3);
// uOffsetY lifts the split composition (the flipped halves sit below center).
struct CrystalSceneFrame {
    double progress = 0.0;
    bool opening = true;
    double split = 0.0;
    double angle = 0.0;
    double offset_x = 0.0;
    double offset_y = 0.0;
};

// GtkGLArea-backed renderer for the Broken Seal crystal shard. Draws a
// fullscreen triangle with the crystal fragment shader, animating the crystal
// emergence (scale-pop + fade-in) and the split (halves rotate + drift) from a
// per-frame scene value. Frames are queued explicitly; the widget is
// transparent and non-interactive.
class CrystalShaderRenderer {
public:
    explicit CrystalShaderRenderer(std::shared_ptr<ShaderManager> shaders);
    ~CrystalShaderRenderer();

    CrystalShaderRenderer(const CrystalShaderRenderer&) = delete;
    CrystalShaderRenderer& operator=(const CrystalShaderRenderer&) = delete;

    [[nodiscard]] GtkWidget* widget() const noexcept;

    // Drives one animation frame from the given scene values.
    void update(const CrystalSceneFrame& frame) noexcept;

    void set_opacity(double opacity) noexcept;

    // True once the renderer has drawn at least one frame successfully.
    [[nodiscard]] bool frame_rendered() const noexcept;

    // Clears the active frame state and hides the widget.
    void finish() noexcept;

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::ui::lockscreen
