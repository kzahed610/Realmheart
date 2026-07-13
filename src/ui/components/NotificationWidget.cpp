#include "ui/components/NotificationWidget.hpp"

namespace realmheart::ui::components {

NotificationWidget::NotificationWidget(services::NotificationHistory& history)
    : history_(history) {
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(box_, "realmheart-notifications");
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    state_->owner = this;
    const auto state = state_;
    subscription_ = history_.subscribe([state](const auto&) {
        if (!state->alive.load() || state->refresh_queued.exchange(true)) return;
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* lifetime = static_cast<std::shared_ptr<LifetimeState>*>(raw);
                (*lifetime)->refresh_queued = false;
                if ((*lifetime)->alive.load() && (*lifetime)->owner != nullptr) {
                    (*lifetime)->owner->refresh();
                }
                return G_SOURCE_REMOVE;
            },
            new std::shared_ptr<LifetimeState>(state),
            +[](gpointer raw) { delete static_cast<std::shared_ptr<LifetimeState>*>(raw); }
        );
    });
    refresh();
}

NotificationWidget::~NotificationWidget() {
    subscription_.reset();
    state_->alive = false;
    state_->owner = nullptr;
}

GtkWidget* NotificationWidget::get_widget() {
    return box_;
}

void NotificationWidget::refresh() {
    GtkWidget* child = gtk_widget_get_first_child(box_);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box_), child);
        child = next;
    }

    const auto snapshot = history_.snapshot();
    GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* heading = gtk_label_new("Notifications");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0F);
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_box_append(GTK_BOX(controls), heading);

    GtkWidget* mark_read = gtk_button_new_with_label("Mark read");
    gtk_widget_set_sensitive(mark_read, snapshot.unread_count > 0);
    g_signal_connect(mark_read, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<services::NotificationHistory*>(data)->mark_all_read();
    }), &history_);
    gtk_box_append(GTK_BOX(controls), mark_read);

    GtkWidget* clear = gtk_button_new_with_label("Clear");
    gtk_widget_set_sensitive(clear, !snapshot.entries.empty());
    g_signal_connect(clear, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<services::NotificationHistory*>(data)->clear();
    }), &history_);
    gtk_box_append(GTK_BOX(controls), clear);
    gtk_box_append(GTK_BOX(box_), controls);

    if (snapshot.entries.empty()) {
        GtkWidget* empty = gtk_label_new("No notifications");
        gtk_widget_add_css_class(empty, "realmheart-notification-body");
        gtk_widget_set_margin_top(empty, 10);
        gtk_box_append(GTK_BOX(box_), empty);
        return;
    }

    for (auto iterator = snapshot.entries.rbegin(); iterator != snapshot.entries.rend(); ++iterator) {
        const auto& entry = *iterator;
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_add_css_class(row, "realmheart-notification-row");
        gtk_widget_set_margin_bottom(row, 8);

        GtkWidget* summary = gtk_label_new(entry.summary.c_str());
        gtk_label_set_xalign(GTK_LABEL(summary), 0.0F);
        gtk_label_set_wrap(GTK_LABEL(summary), TRUE);
        if (entry.unread) {
            gchar* markup = g_markup_printf_escaped("<b>%s</b>", entry.summary.c_str());
            gtk_label_set_markup(GTK_LABEL(summary), markup);
            g_free(markup);
        }
        gtk_box_append(GTK_BOX(row), summary);

        if (!entry.body.empty()) {
            GtkWidget* body = gtk_label_new(entry.body.c_str());
            gtk_widget_add_css_class(body, "realmheart-notification-body");
            gtk_label_set_xalign(GTK_LABEL(body), 0.0F);
            gtk_label_set_wrap(GTK_LABEL(body), TRUE);
            gtk_widget_set_margin_start(body, 10);
            gtk_box_append(GTK_BOX(row), body);
        }

        GtkWidget* dismiss = gtk_button_new_with_label("Dismiss");
        g_object_set_data(G_OBJECT(dismiss), "realmheart-notification-id", GUINT_TO_POINTER(entry.id));
        g_signal_connect(dismiss, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
            const auto id = GPOINTER_TO_UINT(
                g_object_get_data(G_OBJECT(button), "realmheart-notification-id")
            );
            static_cast<services::NotificationHistory*>(data)->dismiss(id);
        }), &history_);
        gtk_widget_set_halign(dismiss, GTK_ALIGN_END);
        gtk_box_append(GTK_BOX(row), dismiss);

        gtk_box_append(GTK_BOX(box_), row);
    }
}

} // namespace realmheart::ui::components
