#include "ui/components/NotificationWidget.hpp"
#include "services/Notifications.hpp"
#include <gtk/gtk.h>

namespace realmheart::ui::components {

NotificationWidget::NotificationWidget(services::NotificationHistory& history)
    : history_(history) {
    
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    // Initialize provider once
    provider_ = gtk_css_provider_new();
    gtk_style_context_add_provider(gtk_widget_get_style_context(box_), 
                                   GTK_STYLE_PROVIDER(provider_), 
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    refresh();
}

GtkWidget* NotificationWidget::get_widget() {
    return box_;
}

void NotificationWidget::refresh() {
    GtkWidget* child = gtk_widget_get_first_child(box_);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box_), child);
        child = next;
    }

    auto snapshot = history_.snapshot();
    if (snapshot.entries.empty()) {
        GtkWidget* empty_lbl = gtk_label_new("No notifications");
        gtk_widget_set_margin_top(empty_lbl, 10);
        gtk_box_append(GTK_BOX(box_), empty_lbl);
        return;
    }

    for (const auto& entry : snapshot.entries) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_bottom(row, 8);

        GtkWidget* summary = gtk_label_new(entry.summary.c_str());
        gtk_label_set_xalign(GTK_LABEL(summary), 0.0);
        if (entry.unread) {
            gchar* markup = g_markup_printf_escaped("<b>%s</b>", entry.summary.c_str());
            gtk_label_set_markup(GTK_LABEL(summary), markup);
            g_free(markup);
        }
        gtk_box_append(GTK_BOX(row), summary);

        GtkWidget* body = gtk_label_new(entry.body.c_str());
        gtk_label_set_xalign(GTK_LABEL(body), 0.0);
        gtk_widget_set_margin_start(body, 10);
        gtk_box_append(GTK_BOX(box_), body);

        gtk_box_append(GTK_BOX(box_), row);
    }
}

void NotificationWidget::apply_theme(const services::Palette& palette) {
    std::string text_color = palette.get("text", "#cdd6f4");
    
    std::string css = ".notification-widget { color: " + text_color + "; }";
    
    gtk_css_provider_load_from_string(provider_, css.c_str());
    
    gtk_widget_add_css_class(box_, "notification-widget");
}

} // namespace realmheart::ui::components
