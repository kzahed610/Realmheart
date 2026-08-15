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
    bool active() const noexcept { return state_.active; }
    bool occupied() const noexcept { return state_.windows > 0; }
    void update(const services::WorkspaceState& state);
    void set_morph_suppressed(bool suppressed);
    void set_morph_visual_opacity(double opacity);
    [[nodiscard]] bool morph_suppressed() const noexcept {
        return morph_suppressed_;
    }
    [[nodiscard]] bool compute_artwork_bounds(
        GtkWidget* target,
        graphene_rect_t* bounds
    ) const;

private:
    static void draw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data);
    void rebuild_preview();
    void show_preview();
    void schedule_preview_hide();
    void cancel_preview_hide();
    void set_hovered(bool hovered);
    void start_hover_animation();
    static gboolean animate_hover(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer data);

    GtkWidget* button_ = nullptr;
    GtkWidget* drawing_area_ = nullptr;
    GtkWidget* popover_ = nullptr;
    GtkWidget* preview_box_ = nullptr;
    services::WorkspaceState state_;
    std::function<void(int)> on_activate_;
    std::function<void()> on_right_click_;
    std::function<void(GtkPopover*)> request_exclusive_open_;
    guint hide_timer_id_ = 0;
    guint hover_tick_id_ = 0;
    gint64 hover_last_frame_us_ = 0;
    double hover_progress_ = 0.0;
    double hover_target_ = 0.0;
    bool morph_suppressed_ = false;
};

} // namespace realmheart::ui::bar::widgets
