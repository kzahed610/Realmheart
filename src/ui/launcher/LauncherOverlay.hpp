#pragma once

#include "services/LauncherService.hpp"
#include "services/WallpaperService.hpp"

#include <gtk/gtk.h>

#include <cstddef>
#include <vector>

namespace realmheart::ui {

class LauncherOverlay {
public:
    LauncherOverlay(
        GtkApplication* app,
        services::LauncherService& service,
        services::WallpaperService& wallpaper_service
    );
    ~LauncherOverlay();

    void toggle();
    void show();
    void hide();

private:
    void setup_window();
    void setup_ui();
    void refresh_wallpaper();
    void refresh_idle_content();
    void rebuild_recommendations();
    void rebuild_results();
    void on_search_changed();
    void on_result_selected(GtkListBoxRow* row);
    void update_inspector(const services::LauncherResult* result);
    void activate_result(std::size_t index);
    void activate_recommendation(std::size_t index);
    bool handle_key(guint keyval);

    GtkWindow* window_ = nullptr;
    GtkWidget* search_entry_ = nullptr;
    GtkWidget* wallpaper_picture_ = nullptr;
    GtkWidget* recommendations_revealer_ = nullptr;
    GtkWidget* recommendations_box_ = nullptr;
    GtkWidget* results_revealer_ = nullptr;
    GtkWidget* results_list_ = nullptr;
    GtkWidget* inspector_icon_ = nullptr;
    GtkWidget* inspector_kind_ = nullptr;
    GtkWidget* inspector_title_ = nullptr;
    GtkWidget* inspector_subtitle_ = nullptr;
    GtkWidget* inspector_hint_ = nullptr;

    services::LauncherService& service_;
    services::WallpaperService& wallpaper_service_;
    std::vector<services::LauncherResult> current_results_;
    std::vector<services::LauncherResult> recommendations_;
};

} // namespace realmheart::ui
