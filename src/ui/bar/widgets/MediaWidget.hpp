#pragma once

#include "services/MediaService.hpp"
#include "ui/bar/widgets/BarIconButton.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <optional>
#include <string>

struct _GdkPixbuf;
using GdkPixbuf = _GdkPixbuf;

namespace realmheart::ui::bar::widgets {

class MediaWidget {
public:
    MediaWidget(
        GtkApplication* app,
        services::MediaService& media_service,
        std::function<void()> request_exclusive_open
    );
    ~MediaWidget();

    MediaWidget(const MediaWidget&) = delete;
    MediaWidget& operator=(const MediaWidget&) = delete;

    GtkWidget* widget() const { return button_.widget(); }
    void update(const std::optional<services::MediaInfo>& info);
    void close();

private:
    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> art_generation{0};
        MediaWidget* owner = nullptr;
    };

    void toggle();
    void show_layer_window();
    void hide_layer_window();
    int layer_left_margin() const;
    void invoke_control(const char* method);
    void start_position_refresh();
    void stop_position_refresh();
    void refresh_seek_ui();
    void schedule_seek(double seconds);
    void commit_pending_seek();
    std::int64_t estimated_position_us() const;
    void update_art(const std::string& art_url);
    void apply_art(const std::string& art_url, std::uint64_t generation, GdkPixbuf* pixbuf);

    services::MediaService& media_service_;
    std::function<void()> request_exclusive_open_;
    BarIconButton button_;
    GtkWidget* layer_window_ = nullptr;
    guint reveal_tick_id_ = 0;
    gint64 reveal_started_us_ = 0;
    int reveal_start_x_ = 0;
    int reveal_target_x_ = 0;
    GtkWidget* album_stack_ = nullptr;
    GtkWidget* album_picture_ = nullptr;
    GtkWidget* album_fallback_ = nullptr;
    GtkWidget* title_label_ = nullptr;
    GtkWidget* artist_label_ = nullptr;
    GtkWidget* seek_scale_ = nullptr;
    GtkWidget* position_label_ = nullptr;
    GtkWidget* duration_label_ = nullptr;
    GtkWidget* previous_button_ = nullptr;
    std::unique_ptr<ThemedSvgIcon> previous_icon_;
    GtkWidget* play_pause_button_ = nullptr;
    std::unique_ptr<ThemedSvgIcon> play_pause_icon_;
    GtkWidget* next_button_ = nullptr;
    std::unique_ptr<ThemedSvgIcon> next_icon_;
    std::optional<services::MediaInfo> info_;
    std::string requested_art_url_;
    bool art_request_complete_ = false;
    guint position_refresh_timer_id_ = 0;
    guint seek_commit_timer_id_ = 0;
    bool has_pending_seek_ = false;
    bool updating_seek_ui_ = false;
    double pending_seek_seconds_ = 0.0;
    std::int64_t position_anchor_us_ = 0;
    gint64 position_anchor_monotonic_us_ = 0;
    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
};

} // namespace realmheart::ui::bar::widgets
