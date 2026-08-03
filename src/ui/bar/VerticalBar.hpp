#pragma once

#include "services/BatteryService.hpp"
#include "services/HyprlandEventMonitor.hpp"
#include "services/MediaService.hpp"
#include "services/Notifications.hpp"
#include "services/Wifi.hpp"
#include "ui/bar/WorkspaceWindowTracker.hpp"
#include "ui/bar/widgets/BarBackdrop.hpp"
#include "ui/bar/widgets/BarIconButton.hpp"
#include "ui/bar/widgets/BatteryWidget.hpp"
#include "ui/bar/widgets/ClockWidget.hpp"
#include "ui/bar/widgets/MediaWidget.hpp"
#include "ui/bar/widgets/SystemMonitorWidget.hpp"
#include "ui/bar/widgets/WorkspaceRune.hpp"

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
        std::function<void()> toggle_sidebar,
        std::function<void()> launch_launcher,
        std::function<void()> open_power_menu = {}
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
        std::atomic<bool> media_refresh_pending{false};
        std::atomic<bool> battery_in_flight{false};
        std::atomic<bool> wifi_in_flight{false};
        std::atomic<bool> notification_refresh_queued{false};
        VerticalBar* owner = nullptr; // GTK main thread only
    };

    void setup_layout();
    void populate_widgets();
    void request_workspace_refresh();
    void request_media_refresh();
    void request_battery_refresh();
    void request_wifi_refresh();
    void apply_workspaces(services::WorkspaceSnapshot snapshot);
    void apply_media(const std::optional<services::MediaInfo>& info);
    void apply_battery(const std::optional<services::BatteryStatus>& status);
    void apply_wifi(const std::optional<services::WifiState>& state);
    void apply_notifications(const services::NotificationSnapshot& notifications);
    void open_exclusive_popover(GtkPopover* popover);
    void open_exclusive_media();
    void open_exclusive_system();
    void activate_workspace(int workspace_id);

    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* root_overlay_ = nullptr;
    GtkWidget* content_container_ = nullptr;
    GtkWidget* workspace_box_ = nullptr;
    GtkWidget* workspace_region_ = nullptr;

    std::unique_ptr<widgets::BarBackdrop> backdrop_;
    std::unique_ptr<widgets::BarIconButton> launcher_button_;
    std::unique_ptr<widgets::MediaWidget> media_widget_;
    std::unique_ptr<widgets::SystemMonitorWidget> system_monitor_widget_;
    std::vector<std::unique_ptr<widgets::WorkspaceRune>> workspace_runes_;
    std::unique_ptr<widgets::ClockWidget> clock_;
    std::unique_ptr<widgets::BatteryWidget> battery_widget_;
    std::unique_ptr<widgets::BarIconButton> wifi_button_;
    std::unique_ptr<widgets::BarIconButton> notification_button_;
    std::unique_ptr<widgets::BarIconButton> bottom_action_button_;

    services::NotificationHistory& notification_history_;
    services::BatteryService& battery_service_;
    services::MediaService& media_service_;
    std::function<void()> toggle_sidebar_;
    std::function<void()> launch_launcher_;
    std::function<void()> open_power_menu_;
    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
    services::NotificationHistory::Subscription notification_subscription_;
    services::MediaService::Subscription media_subscription_;
    std::unique_ptr<services::HyprlandEventMonitor> workspace_monitor_;
    WorkspaceWindowTracker workspace_window_tracker_;
    GWeakRef active_popover_ref_{};
    guint refresh_timer_id_ = 0;
    unsigned refresh_tick_ = 0;
};

} // namespace realmheart::ui::bar
