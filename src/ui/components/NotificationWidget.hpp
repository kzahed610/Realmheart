#pragma once

#include "services/Notifications.hpp"
#include "ui/components/BaseWidget.hpp"

namespace realmheart::ui::components {

class NotificationWidget : public BaseWidget {
public:
    explicit NotificationWidget(services::NotificationHistory& history);
    ~NotificationWidget() override = default;

    GtkWidget* get_widget() override;
    void refresh() override;

private:
    GtkWidget* box_ = nullptr;
    services::NotificationHistory& history_;
};

} // namespace realmheart::ui::components
