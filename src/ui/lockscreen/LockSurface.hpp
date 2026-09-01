#pragma once

#include <gtk/gtk.h>

#include <functional>

namespace realmheart::ui::lockscreen {

// Broken Seal lockscreen surface. A layer-shell overlay window
// (namespace realmheart-broken_seal, exclusive keyboard) hosting the scales
// GL scene as the base layer, with the password entry and "BROKEN SEAL"
// title as GTK widgets above it. PAM auth via AuthPam.
class LockSurface {
public:
    explicit LockSurface(
        GtkApplication* app,
        int monitor_index = -1,
        bool interactive = true
    );
    ~LockSurface();

    LockSurface(const LockSurface&) = delete;
    LockSurface& operator=(const LockSurface&) = delete;

    // Invoked when the user authenticates successfully and the surface hides.
    void set_unlocked_callback(std::function<void()> callback);
    void set_unlock_started_callback(std::function<void()> callback);

    [[nodiscard]] GtkWindow* window() const noexcept;

    // Presents the surface, starts the Forming animation, and focuses the
    // password entry.
    void show();

    // Plays the Closing erosion, then hides on completion.
    void hide();
    void hide_immediately();

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool mapped() const noexcept;
    [[nodiscard]] bool interactive() const noexcept;
    [[nodiscard]] int monitor_index() const noexcept;

private:
    void setup_layout();
    void sync_lit();
    void force_transparent_surface();
    gboolean submit_password();
    void start_tick();
    void stop_tick();
    void advance_frame();
    void push_frame();
    // Safety net: completes the unlock even if the closing animation stalls.
    void force_unlock();

    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::ui::lockscreen
