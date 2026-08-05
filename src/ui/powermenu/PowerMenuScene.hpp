#pragma once

#include "ui/powermenu/PowerMenuVideoState.hpp"

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace realmheart::ui::powermenu::animation {
class PowerMenuRippleRenderer;
}

namespace realmheart::ui::powermenu {

class PowerMenuScene {
public:
    PowerMenuScene();
    ~PowerMenuScene();

    PowerMenuScene(const PowerMenuScene&) = delete;
    PowerMenuScene& operator=(const PowerMenuScene&) = delete;

    [[nodiscard]] GtkWidget* widget() const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] const std::string& error_message() const;
    void set_visibility_callback(std::function<void(double)> callback);
    void present(double normalized_origin_x, double normalized_origin_y);
    void dismiss(std::function<void()> on_hidden);
    void hide_immediately();

private:
    static gboolean timer_callback(gpointer user_data);
    static void stream_notify_callback(
        GObject* object,
        GParamSpec* parameter,
        gpointer user_data
    );

    bool ensure_poster();
    void release_poster() noexcept;
    void ensure_ripple_renderer();
    void release_ripple_renderer() noexcept;
    void acquire_media();
    void release_media() noexcept;
    void destroy_media() noexcept;
    void start_media_playback() noexcept;
    void pause_media_playback() noexcept;
    void sync_media_widgets() noexcept;
    [[nodiscard]] bool live_frame_ready() const noexcept;
    void refresh_opening_ripple_source() noexcept;
    void arm_live_handoff() noexcept;
    void update_live_handoff(gint64 frame_time_us) noexcept;
    void cancel_live_handoff(bool keep_ripple) noexcept;
    [[nodiscard]] bool handoff_needs_frame() const noexcept;
    void handle_stream_notify(GtkMediaStream* stream);
    [[nodiscard]] GdkPaintable* transition_source() const noexcept;
    bool try_begin_ripple();
    void finish_ripple() noexcept;
    gboolean on_timer();
    void ensure_tick();
    void stop_tick();
    void apply_frame();
    void publish_visibility();

    GtkWidget* widget_ = nullptr;
    GtkWidget* media_layer_ = nullptr;
    GtkWidget* poster_picture_ = nullptr;
    GtkWidget* video_widget_ = nullptr;
    GdkTexture* poster_texture_ = nullptr;
    GtkMediaStream* media_stream_ = nullptr;
    std::unique_ptr<animation::PowerMenuRippleRenderer> ripple_renderer_;
    std::filesystem::path video_path_;
    std::filesystem::path poster_path_;
    std::string error_message_;
    PowerMenuVideoState state_;
    std::function<void()> on_hidden_;
    std::function<void(double)> visibility_callback_;
    guint tick_callback_id_ = 0;
    gint64 last_frame_time_us_ = 0;
    double ripple_origin_x_ = 0.012;
    double ripple_origin_y_ = 0.94;
    unsigned int ripple_attempts_ = 0;
    bool ripple_pending_ = false;
    bool ripple_fallback_ = false;
    bool media_source_loaded_ = false;
    bool media_playback_started_ = false;
    bool live_video_committed_ = false;
    bool handoff_pending_ = false;
    bool handoff_active_ = false;
    gint64 handoff_started_us_ = 0;
    gint64 last_ripple_media_timestamp_us_ = -1;
    bool final_ripple_frame_requested_ = false;
};

} // namespace realmheart::ui::powermenu
