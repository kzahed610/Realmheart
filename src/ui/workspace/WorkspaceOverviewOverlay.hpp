#pragma once

#include <array>
#include <gtk/gtk.h>
#include <string>

namespace realmheart::ui::workspace {

class WorkspaceOverviewOverlay {
public:
    explicit WorkspaceOverviewOverlay(GtkApplication* app);
    ~WorkspaceOverviewOverlay();

    WorkspaceOverviewOverlay(const WorkspaceOverviewOverlay&) = delete;
    WorkspaceOverviewOverlay& operator=(const WorkspaceOverviewOverlay&) = delete;

    void show();
    void hide();
    void toggle();

    [[nodiscard]] bool visible() const;

private:
    struct RealmAssets {
        cairo_surface_t* background = nullptr;
        cairo_surface_t* character = nullptr;
    };

    static void draw_callback(
        GtkDrawingArea* area,
        cairo_t* cr,
        int width,
        int height,
        gpointer data
    );

    void draw(cairo_t* cr, int width, int height);
    void activate_at(double x, double y);
    bool ensure_assets();
    void release_assets() noexcept;

    GtkWindow* window_ = nullptr;
    GtkWidget* canvas_ = nullptr;
    std::array<RealmAssets, 4> assets_{};
    std::string asset_error_;
    int active_index_ = 1;
    bool assets_attempted_ = false;
};

} // namespace realmheart::ui::workspace
