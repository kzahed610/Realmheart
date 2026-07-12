#include "ui/components/ClockWidget.hpp"
#include <gtk/gtk.h>
#include <ctime>
#include <iostream>

namespace realmheart::ui::components {

ClockWidget::ClockWidget() {
    label_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(label_, "realmheart-bar-clock");
    gtk_widget_set_margin_top(label_, 8);
    gtk_widget_set_margin_bottom(label_, 8);
    
    update_time();
    refresh();
}

GtkWidget* ClockWidget::get_widget() {
    return label_;
}

void ClockWidget::update_time() {
    std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%H\n%M", &local_time);
    gtk_label_set_text(GTK_LABEL(label_), buffer);
}

void ClockWidget::refresh() {
    if (timer_id_ != 0) return;
    
    timer_id_ = g_timeout_add_seconds(60, +[](gpointer data) -> gboolean {
        auto* self = static_cast<ClockWidget*>(data);
        self->update_time();
        return G_SOURCE_CONTINUE;
    }, this);
}

} // namespace realmheart::ui::components
