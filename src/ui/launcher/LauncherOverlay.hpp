#pragma once

#include "services/LauncherService.hpp"

#include <gtk/gtk.h>

#include <cstddef>
#include <vector>

namespace realmheart::ui {

class LauncherOverlay {
public:
    explicit LauncherOverlay(GtkApplication* app, services::LauncherService& service);
    ~LauncherOverlay();

    void toggle();
    void show();
    void hide();

private:
    void setup_window();
    void setup_ui();
    void on_search_changed();
    void activate_result(std::size_t index);

    GtkWindow* window_ = nullptr;
    GtkWidget* search_entry_ = nullptr;
    GtkWidget* results_list_ = nullptr;
    services::LauncherService& service_;
    std::vector<services::LauncherResult> current_results_;
};

} // namespace realmheart::ui
