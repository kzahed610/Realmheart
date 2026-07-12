#pragma once

#include "ui/components/BaseWidget.hpp"
#include <gtk/gtk.h>
#include <string>

namespace realmheart::ui::components {

class ClockWidget : public BaseWidget {
public:
    ClockWidget();
    ~ClockWidget() override = default;

    GtkWidget* get_widget() override;
    void refresh() override;

private:
    GtkWidget* label_ = nullptr;
    guint timer_id_ = 0;
    void update_time();
};

} // namespace realmheart::ui::components
