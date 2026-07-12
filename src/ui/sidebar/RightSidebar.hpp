#pragma once
#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <gtk/gtk.h>

#include "ui/components/BaseWidget.hpp"
#include "services/KeepAwake.hpp"
#include "services/Notifications.hpp"
#include "services/ThemeService.hpp"

namespace realmheart::ui::sidebar {

class SidebarModule : public components::ThemeableWidget {
public:
    explicit SidebarModule(const std::string& label) : label_(label) {}
    virtual ~SidebarModule() = default;
    virtual void init() {}
    const std::string& get_label() const { return label_; }
protected:
    std::string label_;
};

class RightSidebar {
public:
    RightSidebar(GtkApplication* app, services::NotificationHistory& notification_history, std::shared_ptr<services::ThemeService> theme_service);
    ~RightSidebar() = default;
    
    void add_module(std::shared_ptr<components::BaseWidget> module);
    void refresh();
    GtkWidget* get_window() const { return window_; }

private:
    void setup_layout();
    void populate_modules();

    GtkApplication* app_;
    GtkWidget* window_ = nullptr;
    GtkWidget* container_ = nullptr;
    std::vector<std::shared_ptr<SidebarModule>> modules_;
    std::shared_ptr<services::KeepAwake> keep_awake_;
    services::NotificationHistory& notification_history_;
    
    std::shared_ptr<services::ThemeService> theme_service_;
};

} // namespace realmheart::ui::sidebar
