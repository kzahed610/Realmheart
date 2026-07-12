#pragma once

#include "services/KeepAwake.hpp"
#include "services/Notifications.hpp"
#include "ui/components/BaseWidget.hpp"

#include <gtk/gtk.h>
#include <memory>
#include <vector>

namespace realmheart::ui::sidebar {

class RightSidebar {
public:
    RightSidebar(
        GtkApplication* app,
        services::NotificationHistory& notification_history
    );
    ~RightSidebar();

    RightSidebar(const RightSidebar&) = delete;
    RightSidebar& operator=(const RightSidebar&) = delete;

    void add_module(std::unique_ptr<components::BaseWidget> module);
    void refresh();
    GtkWidget* get_window() const { return window_; }

private:
    void setup_layout();
    void populate_modules();

    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* container_ = nullptr;
    std::vector<std::unique_ptr<components::BaseWidget>> modules_;
    std::shared_ptr<services::KeepAwake> keep_awake_;
    services::NotificationHistory& notification_history_;
};

} // namespace realmheart::ui::sidebar
