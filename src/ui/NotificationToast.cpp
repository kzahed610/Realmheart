#include "ui/NotificationToast.hpp"

#include "ui/LayerSurface.hpp"

#include <algorithm>

namespace realmheart::ui {

NotificationToast::NotificationToast(GtkApplication* app) : app_(app) {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_start(box, 15);
    gtk_widget_set_margin_end(box, 15);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);

    label_summary_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label_summary_), 0.0F);
    gtk_widget_set_halign(label_summary_, GTK_ALIGN_START);

    label_body_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label_body_), 0.0F);
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
    destroying_ = true;
    queue_.clear();
    hide_current();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void NotificationToast::show(const services::NotificationEntry& entry, int timeout_ms) {
    constexpr std::size_t max_queue = 20;
    if (queue_.size() >= max_queue) queue_.pop_front();
    queue_.push_back({entry, timeout_ms});
    if (!visible_) show_next();
}

void NotificationToast::show_next() {
    if (destroying_ || visible_ || queue_.empty()) return;

    QueuedToast toast = std::move(queue_.front());
    queue_.pop_front();
    gtk_label_set_text(GTK_LABEL(label_summary_), toast.entry.summary.c_str());
    gtk_label_set_text(GTK_LABEL(label_body_), toast.entry.body.c_str());
    gtk_window_present(GTK_WINDOW(window_));
    visible_ = true;

    if (toast.timeout_ms > 0) {
        timeout_id_ = g_timeout_add(
            static_cast<guint>(toast.timeout_ms),
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<NotificationToast*>(data);
                self->timeout_id_ = 0;
                self->dismiss();
                return G_SOURCE_REMOVE;
            },
            this
        );
    }
}

void NotificationToast::hide_current() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    visible_ = false;
    if (window_ != nullptr) gtk_widget_set_visible(window_, FALSE);
}

void NotificationToast::dismiss() {
    hide_current();
    if (!destroying_) show_next();
}

} // namespace realmheart::ui
