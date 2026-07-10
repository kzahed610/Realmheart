#include "ui/NotificationToast.hpp"
#include "ui/LayerSurface.hpp"
#include <iostream>

namespace realmheart::ui {

NotificationToast::NotificationToast(GtkApplication* app) : app_(app) {
    window_ = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(box, 15);
    gtk_widget_set_margin_end(box, 15);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    
    label_summary_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label_summary_), 0.0);
    gtk_widget_set_halign(label_summary_, GTK_ALIGN_START);
    // We'll apply bolding via CSS later, for now just raw
    
    label_body_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label_body_), 0.0);
    gtk_widget_set_halign(label_body_, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(label_body_), TRUE);

    gtk_box_append(GTK_BOX(box), label_summary_);
    gtk_box_append(GTK_BOX(box), label_body_);
    gtk_window_set_child(GTK_WINDOW(window_), box);

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-notification";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.margin_right = 20;
    spec.margin_top = 20;
    apply_layer_surface(GTK_WINDOW(window_), spec);
}

NotificationToast::~NotificationToast() {
    dismiss();
    gtk_window_destroy(GTK_WINDOW(window_));
}

void NotificationToast::show(const services::NotificationEntry& entry, int timeout_ms) {
    dismiss();
    
    gtk_label_set_text(GTK_LABEL(label_summary_), entry.summary.c_str());
    gtk_label_set_text(GTK_LABEL(label_body_), entry.body.c_str());
    
    gtk_window_present(GTK_WINDOW(window_));
    
    if (timeout_ms > 0) {
        timeout_id_ = g_timeout_add(timeout_ms, (GSourceFunc)+[](gpointer data) -> gboolean {
            auto* self = static_cast<NotificationToast*>(data);
            self->dismiss();
            return FALSE;
        }, this);
    }
}

void NotificationToast::dismiss() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    gtk_widget_set_visible(window_, FALSE);
}

} // namespace realmheart::ui
