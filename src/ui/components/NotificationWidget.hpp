#pragma once

#include "services/Notifications.hpp"
#include "ui/components/BaseWidget.hpp"

#include <atomic>
#include <memory>

namespace realmheart::ui::components {

class NotificationWidget : public BaseWidget {
public:
    explicit NotificationWidget(services::NotificationHistory& history);
    ~NotificationWidget() override;

    GtkWidget* get_widget() override;
    void refresh() override;

private:
    struct LifetimeState {
        std::atomic<bool> alive{true};
        NotificationWidget* owner = nullptr; // GTK main thread only
        std::atomic<bool> refresh_queued{false};
    };

    GtkWidget* box_ = nullptr;
    services::NotificationHistory& history_;
    std::shared_ptr<LifetimeState> state_ = std::make_shared<LifetimeState>();
    services::NotificationHistory::Subscription subscription_;
};

} // namespace realmheart::ui::components
