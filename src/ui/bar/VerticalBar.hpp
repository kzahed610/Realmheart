#pragma once

#include "services/BatteryService.hpp"
#include "services/MediaService.hpp"
#include "services/HyprlandEventMonitor.hpp"
#include "services/Notifications.hpp"
#include "ui/components/ClockWidget.hpp"
#include "ui/components/StatusWidget.hpp"
#include "ui/components/WorkspacePill.hpp"

#include <atomic>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <optional>
#include <vector>

namespace realmheart::ui::bar {

class VerticalBar {
public:
    VerticalBar(
        GtkApplication* app,
        services::NotificationHistory& notification_history,
        services::BatteryService& battery_service,
        services::MediaService& media_service,
        std::function<void()> toggle_sidebar
    );
    ~VerticalBar();

    VerticalBar(const VerticalBar&) = delete;
    VerticalBar& operator=(const VerticalBar&) = delete;

    GtkWidget* get_window() const { return window_; }
    void refresh();

private:
    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<bool> workspace_in_flight{false};
        std::atomic<bool> workspace_refresh_pending{false};
        std::atomic<bool> media_in_flight{false};
        std::atomic<bool> battery_in_flight{false};
        VerticalBar* owner = nullptr; // GTK main thread only
    };

    void setup_layout();
    void populate_widgets();
    void request_workspace_refresh();
    void request_media_refresh();
    void request_battery_refresh();
    void apply_workspaces(const services::WorkspaceSnapshot& snapshot);
    void apply_media(const std::optional<services::MediaInfo>& info);
    void apply_battery(const std::optional<services::BatteryStatus>& status);
    void apply_notifications(const services::NotificationSnapshot& notifications);

    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* root_container_ = nullptr;
    GtkWidget* workspace_box_ = nullptr;
    GtkWidget* status_box_ = nullptr;

    std::unique_ptr<components::ClockWidget> clock_;
    std::vector<std::unique_ptr<components::WorkspacePill>> workspace_pills_;
    std::unique_ptr<components::StatusWidget> battery_status_;
    std::unique_ptr<components::StatusWidget> media_status_;
    std::unique_ptr<components::StatusWidget> notification_status_;

    services::NotificationHistory& notification_history_;
    services::BatteryService& battery_service_;
    services::MediaService& media_service_;
    std::function<void()> toggle_sidebar_;
    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
    services::NotificationHistory::Subscription notification_subscription_;
    services::MediaService::Subscription media_subscription_;
    std::unique_ptr<services::HyprlandEventMonitor> workspace_monitor_;
    guint refresh_timer_id_ = 0;
    unsigned refresh_tick_ = 0;
};

} // namespace realmheart::ui::bar
