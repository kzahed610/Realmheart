#include "ui/components/ClockWidget.hpp"

#include <ctime>

namespace realmheart::ui::components {

ClockWidget::ClockWidget() {
    label_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(label_, "realmheart-bar-clock");
    gtk_widget_set_margin_top(label_, 8);
    gtk_widget_set_margin_bottom(label_, 8);
    update_time();
    refresh();
}

ClockWidget::~ClockWidget() {
    if (timer_id_ != 0) {
        g_source_remove(timer_id_);
        timer_id_ = 0;
    }
}

GtkWidget* ClockWidget::get_widget() {
    return label_;
}

void ClockWidget::update_time() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H\n%M", &local_time);
    gtk_label_set_text(GTK_LABEL(label_), buffer);
}

void ClockWidget::refresh() {
    update_time();
    if (timer_id_ != 0) return;
    timer_id_ = g_timeout_add_seconds(60, +[](gpointer data) -> gboolean {
        static_cast<ClockWidget*>(data)->update_time();
        return G_SOURCE_CONTINUE;
    }, this);
}

} // namespace realmheart::ui::components
