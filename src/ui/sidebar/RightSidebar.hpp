#pragma once

#include <gtk/gtk.h>
#include <vector>
#include <memory>
#include <string>
#include <functional>

namespace realmheart::ui::sidebar {

/**
 * SidebarModule is a base class for all functional components of the right sidebar.
 * Each module is responsible for its own UI layout and interaction logic.
 */
class SidebarModule {
public:
    explicit SidebarModule(const std::string& label) : label_(label) {}
    virtual ~SidebarModule() = default;

    // Returns the GTK widget to be added to the sidebar container.
    virtual GtkWidget* get_widget() = 0;

    // Optional: Called periodically to refresh status labels/toggles.
    virtual void refresh() {}

    const std::string& get_label() const { return label_; }

protected:
    std::string label_;
};

class RightSidebar {
public:
    explicit RightSidebar(GtkApplication* app);
    ~RightSidebar() = default;

    // Adds a module to the sidebar and triggers a layout refresh.
    void add_module(std::unique_ptr<SidebarModule> module);

    GtkWidget* get_window() const { return window_; }

private:
    void setup_layout();
    void populate_modules();

    GtkApplication* app_;
    GtkWidget* window_ = nullptr;
    GtkWidget* container_ = nullptr;
    std::vector<std::unique_ptr<SidebarModule>> modules_;
};

} // namespace realmheart::ui::sidebar
