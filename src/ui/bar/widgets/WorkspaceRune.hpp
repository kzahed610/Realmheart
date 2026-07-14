#pragma once

#include "services/HyprlandWorkspaces.hpp"

#include <functional>
#include <gtk/gtk.h>

namespace realmheart::ui::bar::widgets {

class WorkspaceRune {
public:
    WorkspaceRune(
        services::WorkspaceState state,
        std::function<void(int)> on_activate,
        std::function<void()> on_right_click,
        std::function<void(GtkPopover*)> request_exclusive_open
    );
    ~WorkspaceRune();

    WorkspaceRune(const WorkspaceRune&) = delete;
    WorkspaceRune& operator=(const WorkspaceRune&) = delete;

    GtkWidget* widget() const { return button_; }
    int workspace_id() const noexcept { return state_.id; }
    void update(const services::WorkspaceState& state);

private:
    static void draw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data);
    void rebuild_preview();
    void show_preview();
    void schedule_preview_hide();
    void cancel_preview_hide();

    GtkWidget* button_ = nullptr;
    GtkWidget* drawing_area_ = nullptr;
    GtkWidget* popover_ = nullptr;
    GtkWidget* preview_box_ = nullptr;
    services::WorkspaceState state_;
    std::function<void(int)> on_activate_;
    std::function<void()> on_right_click_;
    std::function<void(GtkPopover*)> request_exclusive_open_;
    guint hide_timer_id_ = 0;
};

} // namespace realmheart::ui::bar::widgets
