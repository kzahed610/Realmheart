#pragma once

#include "ui/components/BaseWidget.hpp"

namespace realmheart::ui::components {

class ClockWidget : public BaseWidget {
public:
    ClockWidget();
    ~ClockWidget() override;

    GtkWidget* get_widget() override;
    void refresh() override;

private:
    void update_time();

    GtkWidget* label_ = nullptr;
    guint timer_id_ = 0;
};

} // namespace realmheart::ui::components
