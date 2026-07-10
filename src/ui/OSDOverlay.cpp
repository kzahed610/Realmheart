#include "ui/OSDOverlay.hpp"
#include "ui/LayerSurface.hpp"
#include <iostream>

namespace realmheart::ui {

OSDOverlay::OSDOverlay(GtkApplication* app) : app_(app) {
    window_ = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);

    label_ = gtk_label_new(nullptr);
    gtk_widget_set_halign(label_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(label_, GTK_ALIGN_CENTER);
    
    // We'll set a fixed size for the OSD to keep it centered
    gtk_widget_set_size_request(label_, 200, 80);
    gtk_window_set_child(GTK_WINDOW(window_), label_);

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-osd";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.margin_top = 100;
    apply_layer_surface(GTK_WINDOW(window_), spec);
}

OSDOverlay::~OSDOverlay() {
    dismiss();
    gtk_window_destroy(GTK_WINDOW(window_));
}

void OSDOverlay::show_volume(double percent) {
    dismiss();
    std::string text = "Vol: " + std::to_string((int)percent) + "%";
    update_label(text, "");
}

void OSDOverlay::show_brightness(double percent) {
    dismiss();
    std::string text = "Bri: " + std::to_string((int)percent) + "%";
    update_label(text, "☀");
}

void OSDOverlay::update_label(const std::string& text, const std::string& icon) {
    std::string full_text = icon + " " + text;
    gtk_label_set_text(GTK_LABEL(label_), full_text.c_str());
    gtk_window_present(GTK_WINDOW(window_));

    if (timeout_id_ != 0) g_source_remove(timeout_id_);
    timeout_id_ = g_timeout_add(2000, (GSourceFunc)+[](gpointer data) -> gboolean {
        auto* self = static_cast<OSDOverlay*>(data);
        self->dismiss();
        return FALSE;
    }, this);
}

void OSDOverlay::dismiss() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    gtk_widget_set_visible(window_, FALSE);
}

} // namespace realmheart::ui
