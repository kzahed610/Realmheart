#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

namespace realmheart::ui::bar::widgets {
class ThemedSvgIcon;
}

namespace realmheart::ui {

class OSDOverlay {
public:
    explicit OSDOverlay(
        GtkApplication* app,
        std::function<void(bool)> visibility_changed = {},
        int monitor_index = -1
    );
    ~OSDOverlay();

    void show_volume(double percent);
    void show_brightness(double percent);
    void dismiss();

private:
    void show_value(
        const std::string& label,
        const std::string& icon_path,
        double percent
    );
    void animate_progress_to(double percent, guint duration_ms);
    void stop_progress_animation();
    void schedule_dismiss();

    static gboolean progress_tick(
        GtkWidget* widget,
        GdkFrameClock* frame_clock,
        gpointer data
    );
    static gboolean dismiss_timeout(gpointer data);

    GtkApplication* app_ = nullptr;
    int monitor_index_ = -1;
    std::function<void(bool)> visibility_changed_;
    GtkWidget* window_ = nullptr;
    GtkWidget* reveal_ = nullptr;
    GtkWidget* title_label_ = nullptr;
    GtkWidget* value_label_ = nullptr;
    GtkWidget* progress_ = nullptr;
    std::unique_ptr<bar::widgets::ThemedSvgIcon> icon_;

    guint timeout_id_ = 0;
    guint progress_tick_id_ = 0;
    gint64 progress_animation_start_us_ = 0;
    gint64 progress_animation_duration_us_ = 0;
    double progress_animation_start_ = 0.0;
    double progress_animation_target_ = 0.0;
    double displayed_percent_ = 0.0;
};

} // namespace realmheart::ui
