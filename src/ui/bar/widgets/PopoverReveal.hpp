#pragma once

#include <algorithm>
#include <cmath>
#include <gtk/gtk.h>

namespace realmheart::ui::bar::widgets {
namespace detail {

constexpr const char* kPopoverRevealStateKey = "realmheart-popover-reveal-state";

struct PopoverRevealState {
    GtkPopover* popover = nullptr;
    guint tick_id = 0;
    gint64 started_us = 0;
    int start_x = 0;
    int target_x = 0;
    int target_y = 0;
    double fade_delay_fraction = 0.0;
};

inline void destroy_popover_reveal_state(gpointer raw) {
    auto* state = static_cast<PopoverRevealState*>(raw);
    if (state->tick_id != 0 && state->popover != nullptr) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(state->popover), state->tick_id);
    }
    delete state;
}

inline gboolean animate_popover_reveal(
    GtkWidget* widget,
    GdkFrameClock* frame_clock,
    gpointer raw
) {
    auto* state = static_cast<PopoverRevealState*>(raw);
    if (!gtk_widget_get_visible(widget)) {
        state->tick_id = 0;
        gtk_widget_set_opacity(widget, 1.0);
        gtk_popover_set_offset(state->popover, state->target_x, state->target_y);
        g_object_set_data_full(G_OBJECT(widget), kPopoverRevealStateKey, nullptr, nullptr);
        return G_SOURCE_REMOVE;
    }

    const gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
    if (state->started_us == 0) state->started_us = now;

    constexpr double kDurationUs = 210000.0;
    const double linear = std::clamp(
        static_cast<double>(now - state->started_us) / kDurationUs,
        0.0,
        1.0
    );
    const double eased = 1.0 - std::pow(1.0 - linear, 3.0);
    const int x = state->start_x + static_cast<int>(std::lround(
        static_cast<double>(state->target_x - state->start_x) * eased
    ));

    const double opacity_linear = std::clamp(
        (linear - state->fade_delay_fraction) /
            std::max(0.001, 1.0 - state->fade_delay_fraction),
        0.0,
        1.0
    );
    const double opacity_eased = 1.0 - std::pow(1.0 - opacity_linear, 3.0);

    gtk_widget_set_opacity(widget, opacity_eased);
    gtk_popover_set_offset(state->popover, x, state->target_y);

    if (linear >= 1.0) {
        state->tick_id = 0;
        gtk_widget_set_opacity(widget, 1.0);
        gtk_popover_set_offset(state->popover, state->target_x, state->target_y);
        g_object_set_data_full(G_OBJECT(widget), kPopoverRevealStateKey, nullptr, nullptr);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

} // namespace detail

inline constexpr int kExpandingPopoverHiddenTravelPixels = 5;

// Convert an amount of hidden slide travel into the matching timeline delay for
// the cubic ease-out used above. This keeps the complete popover transparent
// until it has physically cleared the rail, rather than guessing a time fraction.
inline double popover_fade_delay_after_travel(
    int slide_pixels,
    int hidden_travel_pixels
) {
    const int safe_slide_pixels = std::max(0, slide_pixels);
    if (safe_slide_pixels == 0) return 0.0;

    const double hidden_motion_fraction = std::clamp(
        static_cast<double>(std::max(0, hidden_travel_pixels)) /
            static_cast<double>(safe_slide_pixels),
        0.0,
        0.95
    );
    return 1.0 - std::cbrt(1.0 - hidden_motion_fraction);
}

inline void reveal_popover(
    GtkPopover* popover,
    int target_x,
    int target_y,
    int slide_pixels = 7,
    double fade_delay_fraction = 0.0
) {
    if (popover == nullptr) return;

    auto* widget = GTK_WIDGET(popover);
    g_object_set_data_full(G_OBJECT(widget), detail::kPopoverRevealStateKey, nullptr, nullptr);

    gboolean animations_enabled = TRUE;
    if (GtkSettings* settings = gtk_widget_get_settings(widget); settings != nullptr) {
        g_object_get(settings, "gtk-enable-animations", &animations_enabled, nullptr);
    }
    if (!animations_enabled) {
        gtk_widget_set_opacity(widget, 1.0);
        gtk_popover_set_offset(popover, target_x, target_y);
        gtk_popover_popup(popover);
        return;
    }

    const int safe_slide_pixels = std::max(0, slide_pixels);
    const double safe_fade_delay = std::clamp(fade_delay_fraction, 0.0, 0.80);
    // The shell begins at the anchor edge and travels only through the rail gutter.
    auto* state = new detail::PopoverRevealState{
        .popover = popover,
        .start_x = target_x - safe_slide_pixels,
        .target_x = target_x,
        .target_y = target_y,
        .fade_delay_fraction = safe_fade_delay,
    };

    gtk_widget_set_opacity(widget, 0.0);
    gtk_popover_set_offset(popover, state->start_x, target_y);
    gtk_popover_popup(popover);

    state->tick_id = gtk_widget_add_tick_callback(
        widget,
        detail::animate_popover_reveal,
        state,
        nullptr
    );
    g_object_set_data_full(
        G_OBJECT(widget),
        detail::kPopoverRevealStateKey,
        state,
        detail::destroy_popover_reveal_state
    );
}

} // namespace realmheart::ui::bar::widgets
