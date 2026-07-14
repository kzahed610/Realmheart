#include "ui/bar/widgets/ClockWidget.hpp"

#include <chrono>
#include <ctime>
#include <string>

namespace realmheart::ui::bar::widgets {

ClockWidget::ClockWidget() {
    label_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(label_, "realmheart-bar-clock");
    refresh();
}

ClockWidget::~ClockWidget() {
    if (timer_id_ != 0) {
        g_source_remove(timer_id_);
        timer_id_ = 0;
    }
}

void ClockWidget::update_time() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%I:%M\n%p", &local_time);
    std::string formatted(buffer);
    if (formatted.starts_with('0')) formatted.erase(formatted.begin());
    gtk_label_set_text(GTK_LABEL(label_), formatted.c_str());
}

void ClockWidget::schedule_next_tick() {
    if (timer_id_ != 0) g_source_remove(timer_id_);
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()
    ).count();
    const guint delay_ms = static_cast<guint>((60 - (seconds % 60)) * 1000 + 25);
    timer_id_ = g_timeout_add(
        delay_ms,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<ClockWidget*>(data);
            self->timer_id_ = 0;
            self->update_time();
            self->schedule_next_tick();
            return G_SOURCE_REMOVE;
        },
        this
    );
}

void ClockWidget::refresh() {
    update_time();
    schedule_next_tick();
}

} // namespace realmheart::ui::bar::widgets
