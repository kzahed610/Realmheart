#pragma once

#include <gtk/gtk.h>

#include <memory>

namespace realmheart::ui::lockscreen {

class CrystalShaderRenderer;
class LockStateMachine;
class ShaderManager;

// Fullscreen Broken Seal lockscreen surface. A layer-shell overlay window
// (namespace realmheart-broken_seal, exclusive keyboard) hosting the crystal
// shader renderer plus the "BROKEN SEAL" header, clock, and password entry.
// Owns the 16 ms animation tick loop that drives the state machine.
class LockSurface {
public:
    explicit LockSurface(GtkApplication* app);
    ~LockSurface();

    LockSurface(const LockSurface&) = delete;
    LockSurface& operator=(const LockSurface&) = delete;

    [[nodiscard]] GtkWindow* window() const noexcept;

    // Presents the surface and starts the crystal emergence animation.
    void show();

    // Starts the closing animation; the surface hides when it completes.
    void hide();

    void hide_immediately();

    [[nodiscard]] bool visible() const noexcept;

private:
    void setup_layout();
    void force_transparent_surface();
    void ensure_tick();
    gboolean advance_frame();

    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::ui::lockscreen
