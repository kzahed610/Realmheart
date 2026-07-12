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
    refresh();
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
    if (snapshot.entries.empty()) {
        GtkWidget* empty = gtk_label_new("No notifications");
        gtk_widget_add_css_class(empty, "realmheart-notification-body");
        gtk_widget_set_margin_top(empty, 10);
        gtk_box_append(GTK_BOX(box_), empty);
        return;
    }

    for (const auto& entry : snapshot.entries) {
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

        gtk_box_append(GTK_BOX(box_), row);
    }
}

} // namespace realmheart::ui::components
