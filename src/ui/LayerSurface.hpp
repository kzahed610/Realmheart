#pragma once

#include <gtk/gtk.h>

#include <string>
#include <string_view>

namespace realmheart::ui {

enum class LayerSurfaceLevel {
    Background,
    Bottom,
    Top,
    Overlay,
};

enum class LayerKeyboardMode {
    None,
    Exclusive,
    OnDemand,
};

struct LayerSurfaceSpec {
    std::string surface_namespace;
    LayerSurfaceLevel layer = LayerSurfaceLevel::Top;
    LayerKeyboardMode keyboard_mode = LayerKeyboardMode::None;
    bool anchor_left = false;
    bool anchor_right = false;
    bool anchor_top = false;
    bool anchor_bottom = false;
    // 0 respects existing exclusive zones; -1 ignores them and uses the full
    // output (appropriate for fullscreen shell surfaces such as the overview).
    int exclusive_zone = 0;
    int margin_left = 0;
    int margin_right = 0;
    int margin_top = 0;
    int margin_bottom = 0;
    // -1 uses REALMHEART_MONITOR_INDEX, falling back to monitor 0.
    int monitor_index = -1;
};

LayerSurfaceSpec make_bar_surface_spec(int width);
LayerSurfaceSpec make_wallpaper_surface_spec();
LayerSurfaceSpec make_layer_surface_spec(std::string_view ns, LayerSurfaceLevel level, LayerKeyboardMode keyboard);
LayerSurfaceSpec make_test_surface_spec();
// Fullscreen exclusive overlay for the Broken Seal lockscreen. exclusive_zone
// is -1 so the surface ignores every other surface's exclusive zone.
LayerSurfaceSpec make_lockscreen_surface_spec();
// Returns a referenced monitor; callers must g_object_unref it.
GdkMonitor* resolve_layer_surface_monitor(
    GtkWidget* widget,
    int requested_index = -1
);
void set_layer_surface_level(GtkWindow* window, LayerSurfaceLevel layer);
void apply_layer_surface(GtkWindow* window, const LayerSurfaceSpec& spec);

} // namespace realmheart::ui
