#pragma once

#include <gtk/gtk.h>

#include <string>

namespace realmheart::ui::lockscreen {

// Per-frame scene state pushed into the scales shader.
struct SceneFrame {
    double progress = 0.0; // 0..1 forming (0->1) or closing (1->0)
    bool opening = true;   // true = forming, false = closing
    double reveal = 0.0;   // blob growth, 0..1
    double target = 0.2;   // final blob coverage
    double warn = 0.0;     // wrong-password flash 0..1
    double time_s = 0.0;
    float seed = 0.0F;
    double lit = 0.0;      // 0..1: fraction of scales lit by password input
};

// Full-screen transparent GtkGLArea that renders the Broken Seal scales
// scene. Modeled on the power-menu ripple renderer's GL plumbing: ES 3.0,
// auto_render=FALSE, lazy program compile, per-frame uniforms, fullscreen
// triangle (no VBO).
class ScalesRenderer {
public:
    ScalesRenderer();
    ~ScalesRenderer();

    ScalesRenderer(const ScalesRenderer&) = delete;
    ScalesRenderer& operator=(const ScalesRenderer&) = delete;

    [[nodiscard]] GtkWidget* widget() const noexcept;
    [[nodiscard]] bool active() const noexcept;

    // Loads + validates the scales shader (if needed), compiles the program
    // lazily on first render, and makes the surface visible.
    [[nodiscard]] bool present(std::string* error = nullptr);

    // Pushes the current scene frame and queues a render.
    void update(const SceneFrame& frame) noexcept;

    // Hides the surface and stops rendering.
    void finish() noexcept;

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::ui::lockscreen
