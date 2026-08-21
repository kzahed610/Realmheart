#include "ui/lockscreen/LockSurface.hpp"

#include "ui/lockscreen/CrystalShaderRenderer.hpp"
#include "ui/lockscreen/LockStateMachine.hpp"
#include "ui/lockscreen/ShaderManager.hpp"
#include "ui/LayerSurface.hpp"

#include <gdk/gdk.h>

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
    GtkWidget* header_label = nullptr;

    std::unique_ptr<LockStateMachine> state_machine;
    std::shared_ptr<ShaderManager> shaders;
    std::unique_ptr<CrystalShaderRenderer> crystal;

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

    state_->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(state_->window, "Realmheart Broken Seal");
    gtk_window_set_decorated(state_->window, FALSE);
    gtk_window_set_resizable(state_->window, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(state_->window), "realmheart-broken-seal-window");
    gtk_widget_set_can_target(GTK_WIDGET(state_->window), FALSE);

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
            // Never advertise opacity, or the compositor skips alpha blending
            // and the shader's transparent pixels become a black rectangle.
            if (GdkSurface* surface = gtk_native_get_surface(
                    GTK_NATIVE(widget));
                surface != nullptr) {
                cairo_region_t* region = cairo_region_create();
                gdk_surface_set_opaque_region(surface, region);
                cairo_region_destroy(region);
            }
            state->crystal->set_opacity(1.0);
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

void LockSurface::setup_layout() {
    GtkWidget* overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_widget_set_vexpand(overlay, TRUE);
    gtk_widget_set_can_target(overlay, FALSE);

    GtkWidget* crystal_widget = state_->crystal->widget();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), crystal_widget);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay), crystal_widget, TRUE);

    GtkWidget* header = gtk_label_new(nullptr);
    gtk_label_set_markup(
        GTK_LABEL(header),
        "<span font='Sans 28' weight='bold' letter_spacing='6' "
        "foreground='#C77DFF'>B R O K E N &#160; S E A L</span>"
    );
    gtk_widget_add_css_class(header, "realmheart-broken-seal-header");
    gtk_widget_set_halign(header, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(header, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(header, 150);
    gtk_widget_set_hexpand(header, TRUE);
    gtk_widget_set_can_target(header, FALSE);
    gtk_widget_set_visible(header, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), header);

    GtkWidget* clock = gtk_label_new("");
    gtk_widget_add_css_class(clock, "realmheart-broken-seal-clock");
    gtk_widget_set_halign(clock, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(clock, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(clock, 48);
    gtk_widget_set_can_target(clock, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), clock);

    gtk_window_set_child(state_->window, overlay);
}

void LockSurface::force_transparent_surface() {
    if (state_ == nullptr || state_->window == nullptr) return;
    if (!gtk_widget_get_realized(GTK_WIDGET(state_->window))) return;

    GdkSurface* surface = gtk_native_get_surface(
        GTK_NATIVE(state_->window)
    );
    if (surface == nullptr) return;
    cairo_region_t* region = cairo_region_create();
    gdk_surface_set_opaque_region(surface, region);
    cairo_region_destroy(region);
}

void LockSurface::show() {
    if (state_ == nullptr) return;
    state_->state_machine->present();
    if (state_->header_label != nullptr) {
        gtk_widget_set_visible(state_->header_label, FALSE);
    }
    CrystalSceneFrame frame;
    frame.progress = 0.0;
    frame.opening = true;
    state_->crystal->update(frame);
    state_->crystal->set_opacity(1.0);
    gtk_window_present(state_->window);
    force_transparent_surface();
    ensure_tick();
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

    // Header appears once the horns are in place.
    const bool split_active = phase == LockPhase::Splitting || phase == LockPhase::Typing;
    if (state_->header_label != nullptr) {
        gtk_widget_set_visible(state_->header_label, split_active);
    }

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

} // namespace realmheart::ui::lockscreen
