#pragma once

#include "ui/components/BaseWidget.hpp"
#include <gtk/gtk.h>
#include "services/Notifications.hpp"
#include <string>
#include <vector>

namespace realmheart::ui::components {

class NotificationWidget : public ThemeableWidget {
public:
    NotificationWidget(services::NotificationHistory& history);
    ~NotificationWidget() override = default;

    GtkWidget* get_widget() override;
    void refresh() override;
    void apply_theme(const services::Palette& palette) override;

private:
    GtkWidget* box_ = nullptr;
    services::NotificationHistory& history_;
};

} // namespace realmheart::ui::components
