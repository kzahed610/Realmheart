#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>

namespace realmheart::ui::lockscreen {

// Broken Seal lockscreen surface. The normal path uses the compositor's
// ext-session-lock role; the layer-shell role is retained only for callers
// that explicitly request a non-lock surface. The surface hosts the scales GL
// scene as the base layer, with the password entry and "BROKEN SEAL" title as
// GTK widgets above it. PAM auth via AuthPam.
class LockSurface {
public:
    explicit LockSurface(
        GtkApplication* app,
        int monitor_index = -1,
        bool interactive = true,
        bool session_lock_surface = false
    );
    ~LockSurface();

    LockSurface(const LockSurface&) = delete;
    LockSurface& operator=(const LockSurface&) = delete;

    // Invoked when the user authenticates successfully and the surface hides.
    void set_unlocked_callback(std::function<void()> callback);
    void set_unlock_started_callback(std::function<void()> callback);
    void set_authentication_enabled(bool enabled) noexcept;

    [[nodiscard]] GtkWindow* window() const noexcept;

    // Presents the surface, starts the Forming animation, and focuses the
    // password entry.
    void show();

    // Plays the Closing erosion, then hides on completion.
    void hide();
    void hide_immediately();

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool mapped() const noexcept;
    [[nodiscard]] bool coverage_verified() const noexcept;
    [[nodiscard]] bool interactive() const noexcept;
    [[nodiscard]] bool uses_session_lock() const noexcept;
    [[nodiscard]] int monitor_index() const noexcept;

private:
    void setup_layout();
    void sync_lit();
    void force_transparent_surface();
    void clear_password_entry() noexcept;
    gboolean submit_password();
    void start_tick();
    void stop_tick();
    void advance_frame();
    void push_frame();
    // Safety net: completes the unlock even if the closing animation stalls.
    void force_unlock();

    struct State;
    std::shared_ptr<State> state_;
};

} // namespace realmheart::ui::lockscreen
