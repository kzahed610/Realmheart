#pragma once
#include <memory>
#include "services/KeepAwake.hpp"
#include "services/Notifications.hpp"

#include <gtk/gtk.h>
#include <vector>
#include <memory>
#include <string>
#include <functional>

namespace realmheart::ui::sidebar {

class SidebarModule {
public:
    explicit SidebarModule(const std::string& label) : label_(label) {}
    virtual ~SidebarModule() = default;
    virtual GtkWidget* get_widget() = 0;
    virtual void refresh() {}
    virtual void init() {}
    const std::string& get_label() const { return label_; }
protected:
    std::string label_;
};

class RightSidebar {
public:
    RightSidebar(GtkApplication* app, services::NotificationHistory& notification_history);
    ~RightSidebar() = default;
    void add_module(std::shared_ptr<SidebarModule> module);
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
};

} // namespace realmheart::ui::sidebar
