#include "ui/lockscreen/LockSurface.hpp"

#include "ui/lockscreen/AuthPam.hpp"
#include "ui/lockscreen/ScalesRenderer.hpp"
#include "ui/lockscreen/ScalesStateMachine.hpp"
#include "ui/lockscreen/ShaderManager.hpp"
#include "ui/LayerSurface.hpp"

#include <gdk/gdk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace realmheart::ui::lockscreen {

// The shader's blob coverage (uTarget) — must match the renderer default.
constexpr double kBlobTarget = 0.20;
// The GL scene's seed; stable per process so the field looks identical on
// every lock/unlock cycle.
constexpr float kSceneSeed = 0.618034F;
// Scales lit per typed character, as a fraction of the center-out rank
// range. 12 chars ≈ the full patch lit; anything longer clamps there.
constexpr double kLitPerChar = 0.083;
constexpr size_t kMaxLitChars = 12;

struct LockSurface::State {
    LockSurface* owner = nullptr;
    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;
    GtkWidget* entry = nullptr;
    GtkWidget* error_label = nullptr;
    std::function<void()> unlocked_callback;

    std::unique_ptr<AuthPam> auth;
    std::unique_ptr<ScalesRenderer> scales;
    std::unique_ptr<ShaderManager> shaders;
    std::unique_ptr<ScalesStateMachine> machine;

    guint tick_source_id = 0;
    gint64 last_tick_us = 0;
    // 0..1: how much of the scale field is lit by the current password.
    double lit = 0.0;

    // Idle continues to shimmer: the tick keeps running while visible.
    bool tick_running = false;
    bool closing = false;
    // Watchdog so a stalled closing animation can never leave the user
    // locked out: force-complete + fire the unlock callback.
    guint closing_watchdog_id = 0;

    void start_tick() noexcept {
        if (tick_running) return;
        tick_running = true;
        last_tick_us = g_get_monotonic_time();
    }

    void stop_tick() noexcept {
        tick_running = false;
        if (tick_source_id != 0) {
            g_source_remove(tick_source_id);
            tick_source_id = 0;
        }
    }

    void arm_closing_watchdog() noexcept {
        if (closing_watchdog_id != 0) return;
        closing_watchdog_id = g_timeout_add(
            2500,
            +[](gpointer data) -> gboolean {
                auto* state = static_cast<State*>(data);
                state->closing_watchdog_id = 0;
                std::cerr << "[LockSurface] closing watchdog fired — forcing unlock\n";
                if (state->owner != nullptr) state->owner->force_unlock();
                return G_SOURCE_REMOVE;
            },
            this
        );
    }

    void disarm_closing_watchdog() noexcept {
        if (closing_watchdog_id != 0) {
            g_source_remove(closing_watchdog_id);
            closing_watchdog_id = 0;
        }
    }
};

LockSurface::LockSurface(GtkApplication* app) : state_(new State) {
    state_->owner = this;
    state_->application = app;
    state_->auth = std::make_unique<AuthPam>();
    state_->scales = std::make_unique<ScalesRenderer>();
    state_->shaders = std::make_unique<ShaderManager>();
    state_->machine = std::make_unique<ScalesStateMachine>();

    state_->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(state_->window, "Realmheart Lockscreen");
    gtk_window_set_decorated(state_->window, FALSE);
    gtk_window_set_resizable(state_->window, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(state_->window), "realmheart-broken-seal-window");
    // The window must remain an input target so the password entry can
    // receive keyboard events. Individual non-interactive children opt out.
    apply_layer_surface(state_->window, make_lockscreen_surface_spec());

    g_signal_connect(
        state_->window,
        "map",
        G_CALLBACK(+[](GtkWidget* widget, gpointer data) {
            auto* state = static_cast<State*>(data);
            // Re-assert exclusive keyboard mode on map so Hyprland registers
            // this as a lockscreen (locked=true) and blocks other binds.
            gtk_layer_set_keyboard_mode(
                GTK_WINDOW(widget),
                GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE
            );
            // Never advertise opacity, or the compositor skips alpha blending
            // and the surface's transparent pixels become a black rectangle.
            if (GdkSurface* surface = gtk_native_get_surface(
                    GTK_NATIVE(widget));
                surface != nullptr) {
                cairo_region_t* region = cairo_region_create();
                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                gdk_surface_set_opaque_region(surface, region);
                G_GNUC_END_IGNORE_DEPRECATIONS
                cairo_region_destroy(region);
            }
            // Focus the password entry once the surface is mapped so keys
            // reach it (grab_focus before map is a silent no-op).
            if (state->entry != nullptr && gtk_widget_get_mapped(state->entry)) {
                gtk_widget_grab_focus(state->entry);
            }
        }),
        state_
    );

    setup_layout();
    gtk_widget_set_visible(GTK_WIDGET(state_->window), FALSE);
}

LockSurface::~LockSurface() {
    if (state_ == nullptr) return;
    state_->stop_tick();
    if (state_->window != nullptr) {
        g_signal_handlers_disconnect_by_data(state_->window, state_);
        gtk_window_destroy(state_->window);
    }
    // GL/widget resources are intentionally leaked on process exit per the
    // established renderer lifetime pattern (the GL context is gone by the
    // time destructors run after g_application_run returns).
    state_->scales.release();
    state_->shaders.release();
    delete state_;
    state_ = nullptr;
}

GtkWindow* LockSurface::window() const noexcept {
    return state_ != nullptr ? state_->window : nullptr;
}

void LockSurface::set_unlocked_callback(std::function<void()> callback) {
    if (state_ != nullptr) state_->unlocked_callback = std::move(callback);
}

void LockSurface::setup_layout() {
    GtkWidget* overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_widget_set_vexpand(overlay, TRUE);

    // Base layer: the scales GL scene, filling the window.
    GtkWidget* gl = state_->scales->widget();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), gl);

    // Password entry: an invisible input region centered on the patch —
    // the scales themselves are the visible input (keystrokes light them).
    GtkWidget* entry = gtk_password_entry_new();
    gtk_widget_add_css_class(entry, "realmheart-broken-seal-entry");
    gtk_widget_set_halign(entry, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(entry, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(entry, 260, 44);
    gtk_widget_set_opacity(entry, 0.0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), entry);
    state_->entry = entry;

    // "seal remains" label, hidden until a wrong password.
    GtkWidget* error_label = gtk_label_new("seal remains");
    gtk_widget_add_css_class(error_label, "realmheart-broken-seal-error-label");
    gtk_widget_set_halign(error_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(error_label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(error_label, 72);
    gtk_widget_set_visible(error_label, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), error_label);
    state_->error_label = error_label;

    // "BROKEN SEAL" title: golden, letter-spaced, bottom-center. Part of the
    // GTK layer, not the shader.
    GtkWidget* title = gtk_label_new("BROKEN SEAL");
    gtk_widget_add_css_class(title, "realmheart-broken-seal-title");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(title, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(title, 42);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), title);

    gtk_window_set_child(state_->window, overlay);

    // Enter in the password entry submits; Escape clears it.
    g_signal_connect(
        entry,
        "activate",
        G_CALLBACK(+[](GtkEditable*, gpointer data) -> void {
            auto* state = static_cast<State*>(data);
            state->owner->submit_password();
        }),
        state_
    );
    // Escape clears the entry (window-level key controller).
    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(
        key_controller,
        "key-pressed",
        G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) -> gboolean {
            auto* state = static_cast<State*>(data);
            if (keyval == GDK_KEY_Escape && state->entry != nullptr) {
                gtk_editable_set_text(GTK_EDITABLE(state->entry), "");
                return TRUE;
            }
            return GDK_EVENT_PROPAGATE;
        }),
        state_
    );
    gtk_widget_add_controller(GTK_WIDGET(state_->window), key_controller);

    // Keystroke ignition: any text change (typing, paste, backspace)
    // re-lights the scales — the field itself is the password input.
    g_signal_connect(
        entry,
        "notify::text",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) -> void {
            static_cast<LockSurface::State*>(data)->owner->sync_lit();
        }),
        state_
    );
}

void LockSurface::sync_lit() {
    if (state_ == nullptr || state_->entry == nullptr) return;
    const char* text = gtk_editable_get_text(GTK_EDITABLE(state_->entry));
    const size_t len = text != nullptr ? strlen(text) : 0;
    state_->lit = static_cast<double>(
        std::min(len, kMaxLitChars)) * kLitPerChar;
    if (state_->scales != nullptr && state_->machine != nullptr &&
        state_->machine->phase() != ScalesPhase::Hidden) {
        push_frame();
    }
}

void LockSurface::force_transparent_surface() {
    if (state_ == nullptr || state_->window == nullptr) return;
    if (!gtk_widget_get_realized(GTK_WIDGET(state_->window))) return;

    GdkSurface* surface = gtk_native_get_surface(
        GTK_NATIVE(state_->window)
    );
    if (surface == nullptr) return;
    cairo_region_t* region = cairo_region_create();
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_surface_set_opaque_region(surface, region);
    G_GNUC_END_IGNORE_DEPRECATIONS
    cairo_region_destroy(region);
}

void LockSurface::show() {
    if (state_ == nullptr) return;
    std::cerr << "[LockSurface] show() called" << '\n';

    std::string error;
    if (!state_->scales->present(&error)) {
        // The scene is optional: the input bar must still work without it.
        std::cerr << "[Lockscreen] scales renderer unavailable: " << error << '\n';
    }

    state_->closing = false;
    state_->machine->present();
    state_->lit = 0.0;
    gtk_window_present(state_->window);
    force_transparent_surface();

    // Re-assert exclusive keyboard after present so the compositor routes
    // all keys to this surface (lockscreen grab).
    if (gtk_widget_get_realized(GTK_WIDGET(state_->window))) {
        gtk_layer_set_keyboard_mode(
            state_->window,
            GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE
        );
    }

    // Show and focus the password entry.
    if (state_->entry != nullptr) {
        gtk_widget_set_visible(state_->entry, TRUE);
        gtk_widget_grab_focus(state_->entry);
    }
    if (state_->error_label != nullptr) {
        gtk_widget_set_visible(state_->error_label, FALSE);
    }

    start_tick();
    push_frame();
}

void LockSurface::hide() {
    if (state_ == nullptr || state_->closing) return;
    std::cerr << "[LockSurface] hide() — starting closing animation" << '\n';
    state_->closing = true;
    state_->machine->dismiss();
    // The member start_tick() self-guards: if the tick is already running it
    // is a no-op, if not it adds the timeout source (never trust the flag
    // alone — it can be set without a live source).
    start_tick();
    state_->arm_closing_watchdog();
    push_frame();
}

void LockSurface::hide_immediately() {
    if (state_ == nullptr) return;
    state_->closing = false;
    state_->machine->hide_immediately();
    state_->stop_tick();
    if (state_->scales != nullptr) state_->scales->finish();
    if (state_->window != nullptr &&
        gtk_widget_get_visible(GTK_WIDGET(state_->window))) {
        gtk_widget_set_visible(GTK_WIDGET(state_->window), FALSE);
    }
}

void LockSurface::force_unlock() {
    if (state_ == nullptr) return;
    state_->disarm_closing_watchdog();
    if (state_->closing) {
        state_->closing = false;
        state_->machine->hide_immediately();
        state_->stop_tick();
        if (state_->scales != nullptr) state_->scales->finish();
        if (state_->window != nullptr) {
            gtk_widget_set_visible(GTK_WIDGET(state_->window), FALSE);
        }
        if (state_->unlocked_callback) state_->unlocked_callback();
    }
}

bool LockSurface::visible() const noexcept {
    return state_ != nullptr && state_->window != nullptr &&
        gtk_widget_get_visible(GTK_WIDGET(state_->window));
}

void LockSurface::start_tick() {
    if (state_ == nullptr || state_->tick_running) return;
    state_->start_tick();
    state_->tick_source_id = g_timeout_add(
        16,
        +[](gpointer data) -> gboolean {
            auto* state = static_cast<State*>(data);
            state->owner->advance_frame();
            return G_SOURCE_CONTINUE;
        },
        state_
    );
}

void LockSurface::stop_tick() {
    if (state_ == nullptr) return;
    state_->stop_tick();
}

void LockSurface::advance_frame() {
    if (state_ == nullptr || !state_->tick_running) {
        std::cerr << "[LockSurface] advance_frame skipped (tick not running)"
                  << '\n';
        return;
    }

    // Manual 16 ms frame pacing: monotonic delta, clamped and finite-guarded.
    const gint64 now_us = g_get_monotonic_time();
    double delta = static_cast<double>(now_us - state_->last_tick_us) / 1e6;
    state_->last_tick_us = now_us;
    if (!std::isfinite(delta)) delta = 0.0;
    delta = std::clamp(delta, 0.0, 0.1);

    state_->machine->advance(delta);
    push_frame();

    const bool hidden = state_->machine->phase() == ScalesPhase::Hidden;
    if (state_->closing && hidden) {
        std::cerr << "[LockSurface] closing complete — hiding window, firing unlock callback" << '\n';
        state_->disarm_closing_watchdog();
        // Unlock/dismiss completed: fire the callback, then hide.
        if (state_->unlocked_callback) state_->unlocked_callback();
        state_->closing = false;
        state_->stop_tick();
        if (state_->scales != nullptr) state_->scales->finish();
        if (state_->window != nullptr) {
            gtk_widget_set_visible(GTK_WIDGET(state_->window), FALSE);
        }
        return;
    }

    // Idle keeps shimmering; stop ticking only once the window hides.
    if (!state_->machine->needs_frame() && !state_->closing) {
        state_->stop_tick();
    }
}

void LockSurface::push_frame() {
    if (state_ == nullptr || state_->scales == nullptr) return;

    SceneFrame frame;
    const ScalesPhase phase = state_->machine->phase();
    const double progress = state_->machine->progress();

    frame.opening = phase != ScalesPhase::Closing;
    frame.progress = progress;
    frame.target = kBlobTarget;
    frame.seed = kSceneSeed;
    frame.lit = state_->lit;
    frame.time_s = static_cast<double>(g_get_monotonic_time()) / 1e6;

    switch (phase) {
    case ScalesPhase::Forming:
        frame.reveal = progress;
        break;
    case ScalesPhase::Idle:
        frame.reveal = 1.0;
        break;
    case ScalesPhase::Failing:
        frame.reveal = 1.0;
        frame.warn = progress;
        break;
    case ScalesPhase::Closing:
        frame.reveal = progress;
        break;
    case ScalesPhase::Hidden:
        return;
    }

    state_->scales->update(frame);
}

gboolean LockSurface::submit_password() {
    if (state_ == nullptr || state_->entry == nullptr || state_->auth == nullptr) {
        return TRUE;
    }

    const char* text = gtk_editable_get_text(GTK_EDITABLE(state_->entry));
    if (text == nullptr || *text == '\0') {
        std::cerr << "[Lockscreen] submit ignored: empty entry\n";
        return TRUE;
    }

    // Copy the password for the async worker.
    std::string password(text);
    gtk_editable_set_text(GTK_EDITABLE(state_->entry), "");
    std::cerr << "[Lockscreen] submitted " << password.size()
              << " chars" << std::endl;

    // Resolve the current user.
    const char* user = g_get_user_name();
    std::string username = user != nullptr ? user : "";

    state_->auth->verify_async(
        std::move(username),
        std::move(password),
        [this](bool success) {
            if (!success) {
                std::cerr << "[Lockscreen] authentication failed\n";
                if (state_ == nullptr) return;
                // Red flash + "seal remains", then refocus for another try.
                state_->machine->fail();
                start_tick();
                if (state_->error_label != nullptr) {
                    gtk_widget_set_visible(state_->error_label, TRUE);
                }
                if (state_->entry != nullptr) {
                    gtk_widget_grab_focus(state_->entry);
                }
                return;
            }
            std::cout << "[Lockscreen] authentication succeeded\n";
            if (state_ == nullptr) return;
            // Erode the scales (Closing), then fire the callback only once
            // the surface is hidden — the choreography reverses underneath.
            state_->owner->hide();
        }
    );
    return TRUE;
}

} // namespace realmheart::ui::lockscreen
