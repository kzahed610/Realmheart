#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>

namespace realmheart::ui::bar::widgets {
class ThemedSvgIcon;
}

namespace realmheart::ui {

class NowPlayingOverlay {
public:
    explicit NowPlayingOverlay(GtkApplication* app, int monitor_index = -1);
    ~NowPlayingOverlay();

    NowPlayingOverlay(const NowPlayingOverlay&) = delete;
    NowPlayingOverlay& operator=(const NowPlayingOverlay&) = delete;

    void show(const std::string& title, const std::string& artist);
    void dismiss();
    void set_system_osd_visible(bool visible);

private:
    void schedule_dismiss();
    void animate_top_margin_to(int target_margin);
    void stop_margin_animation();

    static gboolean dismiss_timeout(gpointer data);
    static gboolean margin_tick(
        GtkWidget* widget,
        GdkFrameClock* frame_clock,
        gpointer data
    );

    GtkApplication* app_ = nullptr;
    int monitor_index_ = -1;
    GtkWidget* window_ = nullptr;
    GtkWidget* reveal_ = nullptr;
    GtkWidget* title_label_ = nullptr;
    GtkWidget* artist_label_ = nullptr;
    std::unique_ptr<bar::widgets::ThemedSvgIcon> icon_;

    guint timeout_id_ = 0;
    guint margin_tick_id_ = 0;
    gint64 margin_animation_start_us_ = 0;
    int margin_animation_start_ = 28;
    int margin_animation_target_ = 28;
    int current_top_margin_ = 28;
    bool system_osd_visible_ = false;
};

} // namespace realmheart::ui
