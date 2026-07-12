#pragma once

#include "services/BatteryService.hpp"
#include "services/MediaService.hpp"
#include "services/Notifications.hpp"
#include "ui/components/ClockWidget.hpp"
#include "ui/components/StatusWidget.hpp"
#include "ui/components/WorkspacePill.hpp"

#include <gtk/gtk.h>
#include <functional>
#include <memory>
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
    void setup_layout();
    void populate_widgets();
    void rebuild_workspaces();
    void refresh_statuses();

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
};

} // namespace realmheart::ui::bar
