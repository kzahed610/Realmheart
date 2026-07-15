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
        services::MediaService& media_service,
        std::function<void(GtkPopover*)> request_exclusive_open
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
    void invoke_control(const char* method);
    void update_art(const std::string& art_url);
    void apply_art(const std::string& art_url, std::uint64_t generation, GdkPixbuf* pixbuf);

    services::MediaService& media_service_;
    std::function<void(GtkPopover*)> request_exclusive_open_;
    BarIconButton button_;
    GtkWidget* popover_ = nullptr;
    GtkWidget* album_stack_ = nullptr;
    GtkWidget* album_picture_ = nullptr;
    GtkWidget* album_fallback_ = nullptr;
    GtkWidget* title_label_ = nullptr;
    GtkWidget* artist_label_ = nullptr;
    GtkWidget* previous_button_ = nullptr;
    std::unique_ptr<ThemedSvgIcon> previous_icon_;
    GtkWidget* play_pause_button_ = nullptr;
    std::unique_ptr<ThemedSvgIcon> play_pause_icon_;
    GtkWidget* next_button_ = nullptr;
    std::unique_ptr<ThemedSvgIcon> next_icon_;
    std::optional<services::MediaInfo> info_;
    std::string requested_art_url_;
    bool art_request_complete_ = false;
    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
};

} // namespace realmheart::ui::bar::widgets
