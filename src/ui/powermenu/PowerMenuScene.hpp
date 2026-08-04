#pragma once

#include "ui/powermenu/PowerMenuVideoState.hpp"

#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <string>

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
    void present();
    void dismiss(std::function<void()> on_hidden);
    void hide_immediately();

private:
    static gboolean timer_callback(gpointer user_data);
    static void stream_notify_callback(
        GObject* object,
        GParamSpec* parameter,
        gpointer user_data
    );

    void begin_poster_prewarm();
    void acquire_media();
    void release_media() noexcept;
    void release_poster_stream() noexcept;
    void handle_stream_notify(GtkMediaStream* stream);
    bool capture_poster(GtkMediaStream* stream);
    gboolean on_timer();
    void ensure_tick();
    void stop_tick();
    void apply_frame();
    void publish_visibility();

    GtkWidget* widget_ = nullptr;
    GtkWidget* poster_picture_ = nullptr;
    GtkWidget* video_widget_ = nullptr;
    GdkTexture* poster_texture_ = nullptr;
    GtkMediaStream* poster_stream_ = nullptr;
    GtkMediaStream* media_stream_ = nullptr;
    std::filesystem::path video_path_;
    std::string error_message_;
    PowerMenuVideoState state_;
    std::function<void()> on_hidden_;
    std::function<void(double)> visibility_callback_;
    guint tick_callback_id_ = 0;
    gint64 last_frame_time_us_ = 0;
};

} // namespace realmheart::ui::powermenu