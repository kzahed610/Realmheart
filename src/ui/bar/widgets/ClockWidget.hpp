#pragma once

#include <gtk/gtk.h>

namespace realmheart::ui::bar::widgets {

class ClockWidget {
public:
    ClockWidget();
    ~ClockWidget();

    ClockWidget(const ClockWidget&) = delete;
    ClockWidget& operator=(const ClockWidget&) = delete;

    GtkWidget* widget() const { return label_; }
    void refresh();

private:
    void update_time();
    void schedule_next_tick();

    GtkWidget* label_ = nullptr;
    guint timer_id_ = 0;
};

} // namespace realmheart::ui::bar::widgets
