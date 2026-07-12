#pragma once

#include "ui/components/BaseWidget.hpp"
#include "services/Notifications.hpp"
#include "services/ThemeService.hpp"
#include <gtk/gtk.h>
#include <vector>
#include <memory>
#include <string>
#include <functional>

namespace realmheart::ui::bar {

class VerticalBar {
public:
    VerticalBar(GtkApplication* app, services::NotificationHistory& notification_history, std::shared_ptr<services::ThemeService> theme_service, std::function<void()> toggle_sidebar);
    ~VerticalBar() = default;

    GtkWidget* get_window() const { return window_; }
    void refresh();

private:
    void setup_layout();
    void populate_widgets();

    GtkApplication* app_;
    GtkWidget* window_ = nullptr;
    GtkWidget* root_container_ = nullptr;
    
    std::vector<std::shared_ptr<components::BaseWidget>> widgets_;
    services::NotificationHistory& notification_history_;
    std::function<void()> toggle_sidebar_;
    
    std::shared_ptr<services::ThemeService> theme_service_;
};

} // namespace realmheart::ui::bar

// Entry point for legacy integration - must be in the same namespace or use qualified names
namespace realmheart::ui::bar {
    GtkWidget* present_vertical_bar(GtkApplication* app, services::NotificationHistory& notification_history, std::shared_ptr<services::ThemeService> theme_service, std::function<void()> toggle_sidebar);
}
