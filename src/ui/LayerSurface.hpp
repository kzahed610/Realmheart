#pragma once

#include <gtk/gtk.h>

#include <string>

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
    int exclusive_zone = 0;
    int margin_left = 0;
    int margin_right = 0;
    int margin_top = 0;
    int margin_bottom = 0;
};

LayerSurfaceSpec make_bar_surface_spec(int width);
LayerSurfaceSpec make_layer_surface_spec(std::string_view ns, LayerSurfaceLevel level, LayerKeyboardMode keyboard);
LayerSurfaceSpec make_test_surface_spec();
void apply_layer_surface(GtkWindow* window, const LayerSurfaceSpec& spec);

} // namespace realmheart::ui
