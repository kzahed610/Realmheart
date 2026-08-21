#include "ui/lockscreen/LockSurface.hpp"

#include "ui/lockscreen/AuthPam.hpp"
#include "ui/lockscreen/CrystalShaderRenderer.hpp"
#include "ui/lockscreen/LockStateMachine.hpp"
#include "ui/lockscreen/ShaderManager.hpp"
#include "ui/LayerSurface.hpp"

#include <gdk/gdk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>

namespace realmheart::ui::lockscreen {

constexpr int kFrameIntervalMs = 16;
constexpr double kMaxDeltaSeconds = 0.10;

struct LockSurface::State {
    LockSurface* owner = nullptr;
    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;
    GtkWidget* entry = nullptr;
    GtkWidget* title_label = nullptr;
    std::function<void()> unlocked_callback;

    std::unique_ptr<LockStateMachine> state_machine;
    std::shared_ptr<ShaderManager> shaders;
    std::unique_ptr<CrystalShaderRenderer> crystal;
    std::unique_ptr<AuthPam> auth;

    guint tick_source_id = 0;
    gint64 last_frame_time_us = 0;

    void stop_tick() noexcept {
        if (tick_source_id != 0) {
            g_source_remove(tick_source_id);
            tick_source_id = 0;
        }
        last_frame_time_us = 0;
    }
};

LockSurface::LockSurface(GtkApplication* app) : state_(new State) {
    state_->owner = this;
    state_->application = app;
    state_->state_machine = std::make_unique<LockStateMachine>();
    state_->shaders = std::make_shared<ShaderManager>();
    state_->crystal = std::make_unique<CrystalShaderRenderer>(state_->shaders);
    state_->auth = std::make_unique<AuthPam>();

    state_->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(state_->window, "Realmheart Broken Seal");
    gtk_window_set_decorated(state_->window, FALSE);
    gtk_window_set_resizable(state_->window, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(state_->window), "realmheart-broken-seal-window");
    // The window must remain an input target so the password entry can
    // receive keyboard events. Individual non-interactive children opt out.
    apply_layer_surface(state_->window, make_lockscreen_surface_spec());

    g_signal_connect(
        state_->window,
        "realize",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            static_cast<State*>(data)->crystal->set_opacity(0.0);
        }),
        state_
    );
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
            // and the shader's transparent pixels become a black rectangle.
            if (GdkSurface* surface = gtk_native_get_surface(
                    GTK_NATIVE(widget));
                surface != nullptr) {
                cairo_region_t* region = cairo_region_create();
                G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                gdk_surface_set_opaque_region(surface, region);
                G_GNUC_END_IGNORE_DEPRECATIONS
                cairo_region_destroy(region);
            }
            state->crystal->set_opacity(1.0);
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
    gtk_widget_set_can_target(overlay, FALSE);

    GtkWidget* crystal_widget = state_->crystal->widget();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), crystal_widget);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay), crystal_widget, TRUE);

    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(
        GTK_LABEL(title),
        "<span font='Sans 22' weight='bold' letter_spacing='8' "
        "foreground='#E8C15A'>B R O K E N &#160; S E A L</span>"
    );
    gtk_widget_add_css_class(title, "realmheart-broken-seal-title");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(title, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(title, 42);
    gtk_widget_set_can_target(title, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), title);
    state_->title_label = title;

    // Password entry: sits in the gap between the two horns.
    GtkWidget* entry = gtk_password_entry_new();
    gtk_widget_add_css_class(entry, "realmheart-broken-seal-entry");
    gtk_widget_set_halign(entry, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(entry, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(entry, 180);
    gtk_widget_set_size_request(entry, 260, 44);
    gtk_widget_set_visible(entry, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), entry);
    state_->entry = entry;

    GtkWidget* clock = gtk_label_new("");
    gtk_widget_add_css_class(clock, "realmheart-broken-seal-clock");
    gtk_widget_set_halign(clock, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(clock, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(clock, 100);
    gtk_widget_set_can_target(clock, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), clock);

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
    state_->state_machine->present();
    if (state_->title_label != nullptr) {
        gtk_widget_set_visible(state_->title_label, TRUE);
    }
    CrystalSceneFrame frame;
    frame.progress = 0.0;
    frame.opening = true;
    state_->crystal->update(frame);
    state_->crystal->set_opacity(1.0);
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

    ensure_tick();

    // Show and focus the password entry.
    if (state_->entry != nullptr) {
        gtk_widget_set_visible(state_->entry, TRUE);
        gtk_widget_grab_focus(state_->entry);
    }
}

void LockSurface::hide() {
    if (state_ == nullptr) return;
    if (state_->state_machine->phase() == LockPhase::Hidden) return;
    state_->state_machine->dismiss();
    ensure_tick();
}

void LockSurface::hide_immediately() {
    if (state_ == nullptr) return;
    state_->stop_tick();
    state_->state_machine->hide_immediately();
    state_->crystal->finish();
    if (state_->window != nullptr &&
        gtk_widget_get_visible(GTK_WIDGET(state_->window))) {
        gtk_widget_set_visible(GTK_WIDGET(state_->window), FALSE);
    }
}

bool LockSurface::visible() const noexcept {
    return state_ != nullptr && state_->window != nullptr &&
        gtk_widget_get_visible(GTK_WIDGET(state_->window));
}

void LockSurface::ensure_tick() {
    if (state_ == nullptr || state_->tick_source_id != 0) return;
    state_->tick_source_id = g_timeout_add(
        kFrameIntervalMs,
        +[](gpointer data) -> gboolean {
            auto* state = static_cast<State*>(data);
            if (state->owner == nullptr) return G_SOURCE_REMOVE;
            return state->state_machine->needs_frame()
                ? state->owner->advance_frame()
                : G_SOURCE_REMOVE;
        },
        state_
    );
}

gboolean LockSurface::advance_frame() {
    if (state_ == nullptr) return G_SOURCE_REMOVE;

    const gint64 now = g_get_monotonic_time();
    double delta_seconds = 1.0 / 60.0;
    if (state_->last_frame_time_us != 0) {
        delta_seconds = static_cast<double>(now - state_->last_frame_time_us) /
            1'000'000.0;
    }
    state_->last_frame_time_us = now;
    delta_seconds = std::clamp(delta_seconds, 0.0, kMaxDeltaSeconds);

    state_->state_machine->advance(delta_seconds);

    const LockPhase phase = state_->state_machine->phase();
    const double progress = state_->state_machine->progress();

    // Build the per-frame scene. Opening: horns pop in with easeOutBack.
    // The horns are FIXED — no split/slide animation. Typing happens in the
    // gap between them. Closing: fade/scale out.
    // Only Opening and Closing animate the emergence scale; Splitting/Typing
    // hold at progress 1.0 so the pop-in never runs a second time.
    CrystalSceneFrame frame;
    frame.opening = phase != LockPhase::Closing;
    if (phase == LockPhase::Opening || phase == LockPhase::Closing) {
        frame.progress = progress;
    } else {
        frame.progress = 1.0;
    }

    // Title + entry are shown once by show(); do not toggle them per frame
    // (hiding during Opening would steal focus from the entry).
    state_->crystal->update(frame);

    if (phase == LockPhase::Hidden) {
        state_->stop_tick();
        state_->crystal->finish();
        if (state_->window != nullptr &&
            gtk_widget_get_visible(GTK_WIDGET(state_->window))) {
            gtk_widget_set_visible(GTK_WIDGET(state_->window), FALSE);
        }
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

gboolean LockSurface::submit_password() {
    if (state_ == nullptr || state_->entry == nullptr || state_->auth == nullptr) {
        return TRUE;
    }

    const char* text = gtk_editable_get_text(GTK_EDITABLE(state_->entry));
    if (text == nullptr || *text == '\0') return TRUE;

    // Copy the password for the async worker.
    std::string password(text);
    gtk_editable_set_text(GTK_EDITABLE(state_->entry), "");

    // Resolve the current user.
    const char* user = g_get_user_name();
    std::string username = user != nullptr ? user : "";

    state_->auth->verify_async(
        std::move(username),
        std::move(password),
        [this](bool success) {
            if (!success) {
                std::cerr << "[BrokenSeal] authentication failed\n";
                // Refocus for another attempt.
                if (state_ != nullptr && state_->entry != nullptr) {
                    gtk_widget_grab_focus(state_->entry);
                }
                return;
            }
            std::cout << "[BrokenSeal] authentication succeeded\n";
            if (state_ != nullptr) {
                if (state_->unlocked_callback) state_->unlocked_callback();
                state_->owner->hide();
            }
        }
    );
    return TRUE;
}

} // namespace realmheart::ui::lockscreen
