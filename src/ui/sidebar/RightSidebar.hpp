#pragma once
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
    const std::string& get_label() const { return label_; }
protected:
    std::string label_;
};

class RightSidebar {
public:
    explicit RightSidebar(GtkApplication* app);
    ~RightSidebar() = default;
    void add_module(std::unique_ptr<SidebarModule> module);
    GtkWidget* get_window() const { return window_; }

private:
    void setup_layout();
    void populate_modules();
    void refresh_all_modules();

    GtkApplication* app_;
    GtkWidget* window_ = nullptr;
    GtkWidget* container_ = nullptr;
    std::vector<std::unique_ptr<SidebarModule>> modules_;
};

} // namespace realmheart::ui::sidebar
