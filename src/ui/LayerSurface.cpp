#include "ui/LayerSurface.hpp"

#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <limits>

namespace realmheart::ui {
namespace {

GtkLayerShellLayer to_gtk_layer(LayerSurfaceLevel layer) {
    switch (layer) {
    case LayerSurfaceLevel::Background: return GTK_LAYER_SHELL_LAYER_BACKGROUND;
    case LayerSurfaceLevel::Bottom: return GTK_LAYER_SHELL_LAYER_BOTTOM;
    case LayerSurfaceLevel::Top: return GTK_LAYER_SHELL_LAYER_TOP;
    case LayerSurfaceLevel::Overlay: return GTK_LAYER_SHELL_LAYER_OVERLAY;
    }
    return GTK_LAYER_SHELL_LAYER_TOP;
}

GtkLayerShellKeyboardMode to_gtk_keyboard_mode(LayerKeyboardMode mode) {
    switch (mode) {
    case LayerKeyboardMode::None: return GTK_LAYER_SHELL_KEYBOARD_MODE_NONE;
    case LayerKeyboardMode::Exclusive: return GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE;
    case LayerKeyboardMode::OnDemand: return GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND;
    }
    return GTK_LAYER_SHELL_KEYBOARD_MODE_NONE;
}

int configured_monitor_index() {
    static const int index = [] {
        const char* configured = g_getenv("REALMHEART_MONITOR_INDEX");
        if (configured == nullptr || *configured == '\0') return 0;

        char* end = nullptr;
        const gint64 parsed = g_ascii_strtoll(configured, &end, 10);
        if (end == configured || end == nullptr || *end != '\0' ||
            parsed < 0 || parsed > std::numeric_limits<int>::max()) {
            return 0;
        }
        return static_cast<int>(parsed);
    }();
    return index;
}

} // namespace

GdkMonitor* resolve_layer_surface_monitor(
    GtkWidget* widget,
    int requested_index
) {
    if (widget == nullptr) return nullptr;
    // Some layer surfaces are set up before the GTK widget has a realized native
    // and display. Accessing the monitor list during that window is unsafe and can
    // crash in GTK while resolving scale/factor metadata. Let the compositor pick
    // the monitor naturally until the window is realized.
    if (!gtk_widget_get_realized(widget)) return nullptr;

    GdkDisplay* display = gtk_widget_get_display(widget);
    if (display == nullptr) return nullptr;

    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors == nullptr) return nullptr;
    const guint count = g_list_model_get_n_items(monitors);
    if (count == 0) return nullptr;

    const int configured = requested_index >= 0
        ? requested_index
        : configured_monitor_index();
    const guint index = configured >= 0 && static_cast<guint>(configured) < count
        ? static_cast<guint>(configured)
        : 0U;
    return GDK_MONITOR(g_list_model_get_item(monitors, index));
}

LayerSurfaceSpec make_bar_surface_spec(int width) {
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-bar";
    spec.layer = LayerSurfaceLevel::Top;
    spec.keyboard_mode = LayerKeyboardMode::OnDemand;
    spec.anchor_left = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    spec.exclusive_zone = std::max(width, 0);
    return spec;
}

LayerSurfaceSpec make_wallpaper_surface_spec() {
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-wallpaper";
    spec.layer = LayerSurfaceLevel::Background;
    spec.keyboard_mode = LayerKeyboardMode::None;
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    return spec;
}

LayerSurfaceSpec make_layer_surface_spec(std::string_view ns, LayerSurfaceLevel level, LayerKeyboardMode keyboard) {
    LayerSurfaceSpec spec;
    spec.surface_namespace = std::string(ns);
    spec.layer = level;
    spec.keyboard_mode = keyboard;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    return spec;
}

LayerSurfaceSpec make_test_surface_spec() {
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-test-layer";
    spec.layer = LayerSurfaceLevel::Top;
    spec.keyboard_mode = LayerKeyboardMode::OnDemand;
    spec.anchor_left = true;
    spec.anchor_top = true;
    spec.margin_left = 16;
    spec.margin_top = 16;
    return spec;
}

LayerSurfaceSpec make_lockscreen_surface_spec() {
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-broken_seal";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::Exclusive;
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    spec.exclusive_zone = -1;
    return spec;
}

void set_layer_surface_level(GtkWindow* window, LayerSurfaceLevel layer) {
    if (window == nullptr) return;
    gtk_layer_set_layer(window, to_gtk_layer(layer));
}

void apply_layer_surface(GtkWindow* window, const LayerSurfaceSpec& spec) {
    gtk_layer_init_for_window(window);
    gtk_layer_set_namespace(window, spec.surface_namespace.c_str());
    if (GdkMonitor* monitor = resolve_layer_surface_monitor(
            GTK_WIDGET(window),
            spec.monitor_index
        )) {
        gtk_layer_set_monitor(window, monitor);
        g_object_unref(monitor);
    }
    gtk_layer_set_layer(window, to_gtk_layer(spec.layer));
    gtk_layer_set_keyboard_mode(window, to_gtk_keyboard_mode(spec.keyboard_mode));
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, spec.anchor_left);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, spec.anchor_right);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, spec.anchor_top);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, spec.anchor_bottom);
    // The layer-shell protocol reserves -1 for surfaces such as lock screens
    // that must ignore every other surface's exclusive zone. Preserve that
    // sentinel while still rejecting values below the protocol range.
    gtk_layer_set_exclusive_zone(window, std::max(spec.exclusive_zone, -1));
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, std::max(spec.margin_left, 0));
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_RIGHT, std::max(spec.margin_right, 0));
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, std::max(spec.margin_top, 0));
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_BOTTOM, std::max(spec.margin_bottom, 0));
}

} // namespace realmheart::ui
