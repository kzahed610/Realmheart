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

    void clear_notifications();
    void dismiss_notification(std::uint32_t id);
    void begin_drag_scroll(
        GtkGestureDrag* gesture,
        double start_x,
        double start_y
    );
    void update_drag_scroll(GtkGestureDrag* gesture, double offset_x, double offset_y);
    void end_drag_scroll();

    GtkWidget* box_ = nullptr;
    GtkWidget* scroller_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* count_label_ = nullptr;
    GtkWidget* clear_button_ = nullptr;
    GtkAdjustment* vertical_adjustment_ = nullptr;
    double drag_start_value_ = 0.0;
    bool drag_active_ = false;
    bool drag_blocked_ = false;
    services::NotificationHistory& history_;
    std::shared_ptr<LifetimeState> state_ = std::make_shared<LifetimeState>();
    services::NotificationHistory::Subscription subscription_;
};

} // namespace realmheart::ui::components
