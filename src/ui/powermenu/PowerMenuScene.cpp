#include "ui/powermenu/PowerMenuScene.hpp"

#include "ui/AssetResolver.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>

namespace realmheart::ui::powermenu {
namespace {

constexpr const char* kVideoAsset = "power-menu/realmheart-power-menu.mp4";
constexpr guint kFrameIntervalMs = 16;

void configure_layer(GtkWidget* widget) {
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_valign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(widget, FALSE);
}

} // namespace

PowerMenuScene::PowerMenuScene() {
    widget_ = gtk_overlay_new();
    g_object_ref_sink(widget_);
    configure_layer(widget_);
    gtk_widget_set_opacity(widget_, 0.0);

    GtkWidget* backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    configure_layer(backdrop);
    gtk_widget_add_css_class(backdrop, "realmheart-power-video-backdrop");
    gtk_overlay_set_child(GTK_OVERLAY(widget_), backdrop);

    poster_picture_ = gtk_picture_new();
    configure_layer(poster_picture_);
    gtk_picture_set_can_shrink(GTK_PICTURE(poster_picture_), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(poster_picture_), GTK_CONTENT_FIT_COVER);
    gtk_overlay_add_overlay(GTK_OVERLAY(widget_), poster_picture_);

    video_widget_ = gtk_video_new();
    configure_layer(video_widget_);
    gtk_video_set_autoplay(GTK_VIDEO(video_widget_), FALSE);
    gtk_video_set_loop(GTK_VIDEO(video_widget_), TRUE);
    gtk_video_set_graphics_offload(
        GTK_VIDEO(video_widget_),
        GTK_GRAPHICS_OFFLOAD_ENABLED
    );
    gtk_widget_set_visible(video_widget_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(widget_), video_widget_);

    if (GdkDisplay* display = gdk_display_get_default(); display != nullptr) {
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, R"CSS(
            .realmheart-power-video-backdrop {
                background: #010103;
            }
        )CSS");
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
        g_object_unref(provider);
    }

    const auto resolved = resolve_project_asset(kVideoAsset);
    if (!resolved) {
        error_message_ = std::string{"Unable to resolve "} + kVideoAsset;
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
        return;
    }
    video_path_ = *resolved;
    begin_poster_prewarm();
}

PowerMenuScene::~PowerMenuScene() {
    stop_tick();
    on_hidden_ = {};
    visibility_callback_ = {};
    release_media();
    release_poster_stream();
    if (poster_picture_ != nullptr) {
        gtk_picture_set_paintable(GTK_PICTURE(poster_picture_), nullptr);
    }
    g_clear_object(&poster_texture_);
    if (widget_ != nullptr) {
        g_object_unref(widget_);
        widget_ = nullptr;
    }
    poster_picture_ = nullptr;
    video_widget_ = nullptr;
}

GtkWidget* PowerMenuScene::widget() const { return widget_; }

bool PowerMenuScene::ready() const { return !video_path_.empty(); }

const std::string& PowerMenuScene::error_message() const { return error_message_; }

void PowerMenuScene::set_visibility_callback(std::function<void(double)> callback) {
    visibility_callback_ = std::move(callback);
    publish_visibility();
}

void PowerMenuScene::present() {
    on_hidden_ = {};
    if (!ready()) {
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
        return;
    }
    state_.present();
    acquire_media();
    apply_frame();
    ensure_tick();
}

void PowerMenuScene::dismiss(std::function<void()> on_hidden) {
    on_hidden_ = std::move(on_hidden);
    if (state_.phase() == PowerMenuVideoPhase::Hidden) {
        release_media();
        auto callback = std::move(on_hidden_);
        if (callback) callback();
        return;
    }
    state_.dismiss();
    apply_frame();
    ensure_tick();
}

void PowerMenuScene::hide_immediately() {
    stop_tick();
    on_hidden_ = {};
    state_.hide_immediately();
    release_media();
    apply_frame();
}

gboolean PowerMenuScene::timer_callback(gpointer user_data) {
    auto* self = static_cast<PowerMenuScene*>(user_data);
    return self == nullptr ? G_SOURCE_REMOVE : self->on_timer();
}

void PowerMenuScene::stream_notify_callback(
    GObject* object,
    GParamSpec*,
    gpointer user_data
) {
    auto* self = static_cast<PowerMenuScene*>(user_data);
    if (self != nullptr && GTK_IS_MEDIA_STREAM(object)) {
        self->handle_stream_notify(GTK_MEDIA_STREAM(object));
    }
}

void PowerMenuScene::begin_poster_prewarm() {
    if (!ready() || poster_texture_ != nullptr || poster_stream_ != nullptr) return;

    poster_stream_ = GTK_MEDIA_STREAM(
        gtk_media_file_new_for_filename(video_path_.c_str())
    );
    gtk_media_stream_set_loop(poster_stream_, FALSE);
    gtk_media_stream_set_muted(poster_stream_, TRUE);
    g_signal_connect(
        poster_stream_,
        "notify::prepared",
        G_CALLBACK(&PowerMenuScene::stream_notify_callback),
        this
    );
    g_signal_connect(
        poster_stream_,
        "notify::error",
        G_CALLBACK(&PowerMenuScene::stream_notify_callback),
        this
    );
    gtk_media_stream_pause(poster_stream_);

    if (gtk_media_stream_is_prepared(poster_stream_) ||
        gtk_media_stream_get_error(poster_stream_) != nullptr) {
        handle_stream_notify(poster_stream_);
    }
}

void PowerMenuScene::acquire_media() {
    if (!ready() || media_stream_ != nullptr) return;

    if (poster_stream_ != nullptr) {
        media_stream_ = poster_stream_;
        poster_stream_ = nullptr;
        g_signal_handlers_disconnect_by_data(media_stream_, this);
    } else {
        media_stream_ = GTK_MEDIA_STREAM(
            gtk_media_file_new_for_filename(video_path_.c_str())
        );
    }

    gtk_media_stream_set_loop(media_stream_, TRUE);
    gtk_media_stream_set_muted(media_stream_, TRUE);
    g_signal_connect(
        media_stream_,
        "notify::prepared",
        G_CALLBACK(&PowerMenuScene::stream_notify_callback),
        this
    );
    g_signal_connect(
        media_stream_,
        "notify::error",
        G_CALLBACK(&PowerMenuScene::stream_notify_callback),
        this
    );
    gtk_video_set_media_stream(GTK_VIDEO(video_widget_), media_stream_);

    if (gtk_media_stream_is_prepared(media_stream_)) {
        handle_stream_notify(media_stream_);
    }
    gtk_media_stream_play(media_stream_);
    std::cerr << "[PowerMenuScene] video pipeline acquired: "
              << video_path_.filename().string() << '\n';
}

void PowerMenuScene::release_media() noexcept {
    if (media_stream_ == nullptr) return;

    g_signal_handlers_disconnect_by_data(media_stream_, this);
    gtk_media_stream_pause(media_stream_);
    if (video_widget_ != nullptr) {
        gtk_video_set_media_stream(GTK_VIDEO(video_widget_), nullptr);
        gtk_widget_set_visible(video_widget_, FALSE);
    }
    if (poster_picture_ != nullptr) {
        gtk_widget_set_visible(poster_picture_, TRUE);
    }
    g_clear_object(&media_stream_);
    std::cerr << "[PowerMenuScene] video pipeline released\n";
}

void PowerMenuScene::release_poster_stream() noexcept {
    if (poster_stream_ == nullptr) return;
    g_signal_handlers_disconnect_by_data(poster_stream_, this);
    gtk_media_stream_pause(poster_stream_);
    g_clear_object(&poster_stream_);
}

void PowerMenuScene::handle_stream_notify(GtkMediaStream* stream) {
    if (const GError* error = gtk_media_stream_get_error(stream); error != nullptr) {
        error_message_ = std::string{"Unable to decode power-menu video: "} + error->message;
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
        if (stream == poster_stream_) release_poster_stream();
        if (stream == media_stream_) release_media();
        return;
    }
    if (!gtk_media_stream_is_prepared(stream)) return;

    const bool poster_ready = poster_texture_ != nullptr || capture_poster(stream);
    if (!poster_ready) {
        std::cerr << "[PowerMenuScene] Unable to cache first-frame poster\n";
    }

    if (stream == poster_stream_) {
        release_poster_stream();
        std::cerr << "[PowerMenuScene] bootstrap decoder released"
                  << (poster_ready ? " after poster capture" : " without a poster")
                  << '\n';
        return;
    }
    if (stream == media_stream_) {
        gtk_widget_set_visible(video_widget_, TRUE);
        gtk_widget_set_visible(poster_picture_, FALSE);
    }
}

bool PowerMenuScene::capture_poster(GtkMediaStream* stream) {
    GdkPaintable* image = gdk_paintable_get_current_image(GDK_PAINTABLE(stream));
    if (image == nullptr || !GDK_IS_TEXTURE(image)) {
        g_clear_object(&image);
        return false;
    }

    GdkTexture* source = GDK_TEXTURE(image);
    const int width = gdk_texture_get_width(source);
    const int height = gdk_texture_get_height(source);
    constexpr std::size_t kBytesPerPixel = 4U;
    if (width <= 0 || height <= 0 ||
        static_cast<std::size_t>(width) >
            (std::numeric_limits<std::size_t>::max() / kBytesPerPixel)) {
        g_object_unref(image);
        return false;
    }

    const std::size_t stride = static_cast<std::size_t>(width) * kBytesPerPixel;
    if (static_cast<std::size_t>(height) >
        (std::numeric_limits<std::size_t>::max() / stride)) {
        g_object_unref(image);
        return false;
    }
    const std::size_t byte_count = stride * static_cast<std::size_t>(height);
    auto* pixels = static_cast<std::uint8_t*>(g_malloc(byte_count));
    gdk_texture_download(source, pixels, stride);
    g_object_unref(image);

    GBytes* bytes = g_bytes_new_take(pixels, byte_count);
    GdkTexture* poster = gdk_memory_texture_new(
        width,
        height,
        GDK_MEMORY_DEFAULT,
        bytes,
        stride
    );
    g_bytes_unref(bytes);
    if (poster == nullptr) return false;

    gtk_picture_set_paintable(GTK_PICTURE(poster_picture_), GDK_PAINTABLE(poster));
    g_clear_object(&poster_texture_);
    poster_texture_ = poster;
    std::cerr << "[PowerMenuScene] first-frame poster cached in CPU memory: "
              << width << 'x' << height << '\n';
    return true;
}

gboolean PowerMenuScene::on_timer() {
    if (!state_.needs_frame()) {
        tick_callback_id_ = 0;
        last_frame_time_us_ = 0;
        return G_SOURCE_REMOVE;
    }

    const gint64 frame_time_us = g_get_monotonic_time();
    if (last_frame_time_us_ == 0) {
        last_frame_time_us_ = frame_time_us;
        return G_SOURCE_CONTINUE;
    }
    const double delta_seconds = std::clamp(
        static_cast<double>(frame_time_us - last_frame_time_us_) / 1'000'000.0,
        0.0,
        0.10
    );
    last_frame_time_us_ = frame_time_us;
    state_.advance(delta_seconds);
    apply_frame();

    if (state_.needs_frame()) return G_SOURCE_CONTINUE;

    tick_callback_id_ = 0;
    last_frame_time_us_ = 0;
    if (!state_.media_required()) {
        release_media();
        auto callback = std::move(on_hidden_);
        if (callback) callback();
    }
    return G_SOURCE_REMOVE;
}

void PowerMenuScene::ensure_tick() {
    if (tick_callback_id_ != 0 || !state_.needs_frame()) return;
    last_frame_time_us_ = 0;
    tick_callback_id_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        kFrameIntervalMs,
        &PowerMenuScene::timer_callback,
        this,
        nullptr
    );
}

void PowerMenuScene::stop_tick() {
    if (tick_callback_id_ != 0) g_source_remove(tick_callback_id_);
    tick_callback_id_ = 0;
    last_frame_time_us_ = 0;
}

void PowerMenuScene::apply_frame() {
    if (widget_ != nullptr) gtk_widget_set_opacity(widget_, state_.opacity());
    publish_visibility();
}

void PowerMenuScene::publish_visibility() {
    if (visibility_callback_) visibility_callback_(state_.opacity());
}

} // namespace realmheart::ui::powermenu