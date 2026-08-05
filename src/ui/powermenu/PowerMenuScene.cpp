#include "ui/powermenu/PowerMenuScene.hpp"

#include "ui/AssetResolver.hpp"
#include "ui/powermenu/animation/PowerMenuRippleRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace realmheart::ui::powermenu {
namespace {

constexpr const char* kVideoAsset = "power-menu/realmheart-power-menu.mp4";
constexpr const char* kPosterAsset = "power-menu/realmheart-power-menu-poster.jpg";
constexpr guint kFrameIntervalMs = 16;
constexpr unsigned int kMaximumRippleBootstrapAttempts = 10;
constexpr gint64 kLiveHandoffDurationUs = 140'000;

void configure_layer(GtkWidget* widget) {
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_valign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(widget, FALSE);
}

double sanitize_origin(double value, double fallback) noexcept {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, 0.0, 1.0);
}

} // namespace

PowerMenuScene::PowerMenuScene() {
    widget_ = gtk_overlay_new();
    g_object_ref_sink(widget_);
    configure_layer(widget_);
    gtk_widget_add_css_class(widget_, "realmheart-power-menu-scene");
    gtk_widget_remove_css_class(widget_, "background");
    gtk_widget_set_opacity(widget_, 0.0);

    media_layer_ = gtk_overlay_new();
    configure_layer(media_layer_);
    gtk_widget_add_css_class(media_layer_, "realmheart-power-menu-media");
    gtk_widget_remove_css_class(media_layer_, "background");
    gtk_widget_set_visible(media_layer_, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(widget_), media_layer_);

    poster_picture_ = gtk_picture_new();
    configure_layer(poster_picture_);
    gtk_widget_add_css_class(poster_picture_, "realmheart-power-menu-poster");
    gtk_picture_set_can_shrink(GTK_PICTURE(poster_picture_), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(poster_picture_), GTK_CONTENT_FIT_COVER);
    gtk_overlay_set_child(GTK_OVERLAY(media_layer_), poster_picture_);

    // GtkMediaStream is already a GdkPaintable. Display it through GtkPicture
    // instead of GtkVideo so Realmheart owns playback and the live frame stays
    // in the ordinary GTK scene graph with the exact same COVER geometry as
    // the cached transition poster.
    video_widget_ = gtk_picture_new();
    configure_layer(video_widget_);
    gtk_widget_add_css_class(video_widget_, "realmheart-power-menu-video");
    gtk_picture_set_can_shrink(GTK_PICTURE(video_widget_), TRUE);
    gtk_picture_set_content_fit(
        GTK_PICTURE(video_widget_),
        GTK_CONTENT_FIT_COVER
    );
    gtk_widget_set_visible(video_widget_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(media_layer_), video_widget_);

    if (GdkDisplay* display = gdk_display_get_default(); display != nullptr) {
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, R"CSS(
            .realmheart-power-menu-scene,
            .realmheart-power-menu-media,
            .realmheart-power-menu-poster,
            .realmheart-power-menu-video,
            .realmheart-power-menu-ripple {
                background: transparent;
                background-color: transparent;
            }
        )CSS");
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
        g_object_unref(provider);
    }

    const auto resolved_video = resolve_project_asset(kVideoAsset);
    const auto resolved_poster = resolve_project_asset(kPosterAsset);
    if (!resolved_video || !resolved_poster) {
        error_message_ = "Unable to resolve power-menu media assets";
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
        return;
    }
    video_path_ = *resolved_video;
    poster_path_ = *resolved_poster;
}

PowerMenuScene::~PowerMenuScene() {
    stop_tick();
    on_hidden_ = {};
    visibility_callback_ = {};
    finish_ripple();
    release_ripple_renderer();
    destroy_media();
    release_poster();
    if (widget_ != nullptr) {
        g_object_unref(widget_);
        widget_ = nullptr;
    }
    media_layer_ = nullptr;
    poster_picture_ = nullptr;
    video_widget_ = nullptr;
}

GtkWidget* PowerMenuScene::widget() const { return widget_; }

bool PowerMenuScene::ready() const {
    return !video_path_.empty() && !poster_path_.empty();
}

const std::string& PowerMenuScene::error_message() const { return error_message_; }

void PowerMenuScene::set_visibility_callback(std::function<void(double)> callback) {
    visibility_callback_ = std::move(callback);
    publish_visibility();
}

void PowerMenuScene::present(
    double normalized_origin_x,
    double normalized_origin_y
) {
    on_hidden_ = {};
    if (!ready()) {
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
        return;
    }

    if (!ensure_poster()) {
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
        return;
    }
    ensure_ripple_renderer();

    ripple_origin_x_ = sanitize_origin(normalized_origin_x, 0.012);
    ripple_origin_y_ = sanitize_origin(normalized_origin_y, 0.94);
    cancel_live_handoff(true);
    live_video_committed_ = false;
    state_.present();
    acquire_media();
    // The reveal snapshots the first frame. Do not let the live decoder run
    // underneath it and arrive hundreds of milliseconds ahead at handoff.
    pause_media_playback();

    if (ripple_renderer_ == nullptr || !ripple_renderer_->active()) {
        ripple_pending_ = true;
        ripple_fallback_ = false;
        ripple_attempts_ = 0;
    }

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

    // If the opening handoff is still fading, keep its frozen GL frame and
    // reverse that same texture instead of flashing the live picture first.
    cancel_live_handoff(true);
    state_.dismiss();
    if (ripple_renderer_ == nullptr || !ripple_renderer_->active()) {
        ripple_pending_ = true;
        ripple_fallback_ = false;
        ripple_attempts_ = 0;
    }
    apply_frame();
    ensure_tick();
}

void PowerMenuScene::hide_immediately() {
    stop_tick();
    on_hidden_ = {};
    state_.hide_immediately();
    ripple_pending_ = false;
    ripple_fallback_ = false;
    ripple_attempts_ = 0;
    cancel_live_handoff(false);
    finish_ripple();
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

bool PowerMenuScene::ensure_poster() {
    if (poster_texture_ != nullptr) return true;
    if (poster_path_.empty()) {
        error_message_ = "Power-menu poster path is unavailable";
        return false;
    }

    GError* error = nullptr;
    poster_texture_ = gdk_texture_new_from_filename(
        poster_path_.c_str(),
        &error
    );
    if (poster_texture_ == nullptr) {
        error_message_ = "Unable to load power-menu poster";
        if (error != nullptr && error->message != nullptr) {
            error_message_ += std::string{": "} + error->message;
        }
        g_clear_error(&error);
        return false;
    }

    gtk_picture_set_paintable(
        GTK_PICTURE(poster_picture_),
        GDK_PAINTABLE(poster_texture_)
    );
    std::cerr << "[PowerMenuScene] static poster loaded lazily: "
              << poster_path_.filename().string() << '\n';
    return true;
}

void PowerMenuScene::release_poster() noexcept {
    if (poster_picture_ != nullptr) {
        gtk_picture_set_paintable(GTK_PICTURE(poster_picture_), nullptr);
    }
    g_clear_object(&poster_texture_);
}

void PowerMenuScene::ensure_ripple_renderer() {
    if (ripple_renderer_ != nullptr) return;
    ripple_renderer_ = std::make_unique<animation::PowerMenuRippleRenderer>();
    gtk_overlay_add_overlay(GTK_OVERLAY(widget_), ripple_renderer_->widget());
    std::cerr << "[PowerMenuRipple] GL renderer created lazily\n";
}

void PowerMenuScene::release_ripple_renderer() noexcept {
    if (ripple_renderer_ == nullptr) return;
    GtkWidget* ripple_widget = ripple_renderer_->widget();
    if (ripple_widget != nullptr &&
        gtk_widget_get_parent(ripple_widget) == widget_) {
        gtk_overlay_remove_overlay(GTK_OVERLAY(widget_), ripple_widget);
    }
    ripple_renderer_.reset();
    std::cerr << "[PowerMenuRipple] GL renderer destroyed while idle\n";
}

void PowerMenuScene::acquire_media() {
    if (!ready()) return;

    // Create and source the GtkMediaFile only once. Reassigning its filename on
    // every open causes the GTK GStreamer backend to create another GL worker
    // family even after the old source was cleared.
    if (media_stream_ == nullptr) {
        media_stream_ = GTK_MEDIA_STREAM(gtk_media_file_new());
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
        std::cerr << "[PowerMenuScene] reusable media object created lazily\n";
    }

    if (!media_source_loaded_) {
        gtk_media_file_set_filename(
            GTK_MEDIA_FILE(media_stream_),
            video_path_.c_str()
        );
        media_source_loaded_ = true;
        std::cerr << "[PowerMenuScene] video pipeline created once: "
                  << video_path_.filename().string() << '\n';
    }

    // release_media() detaches the paintable while hidden so GTK has no reason
    // to snapshot the paused stream. Reattach the same pipeline for this use.
    gtk_picture_set_paintable(
        GTK_PICTURE(video_widget_),
        GDK_PAINTABLE(media_stream_)
    );
    pause_media_playback();

    if (gtk_media_stream_is_prepared(media_stream_)) {
        handle_stream_notify(media_stream_);
    } else {
        sync_media_widgets();
    }
}

void PowerMenuScene::release_media() noexcept {
    media_playback_started_ = false;
    live_video_committed_ = false;
    handoff_pending_ = false;
    handoff_active_ = false;
    handoff_started_us_ = 0;
    if (media_layer_ != nullptr) {
        gtk_widget_set_visible(media_layer_, FALSE);
        gtk_widget_set_opacity(media_layer_, 1.0);
    }
    if (media_stream_ == nullptr) return;

    // Do not clear and reopen GtkMediaFile for every power-menu cycle. On the
    // GStreamer GTK backend, every filename assignment creates a fresh GL
    // display/context worker set; gtk_media_file_clear() does not synchronously
    // retire those workers, so repeated cycles accumulate gstglcontext,
    // gldisplay-event and driver threads. Keep the single lazily-created
    // pipeline paused and detached while hidden, then reuse it next time.
    gtk_media_stream_pause(media_stream_);
    if (video_widget_ != nullptr) {
        gtk_picture_set_paintable(GTK_PICTURE(video_widget_), nullptr);
        gtk_widget_set_visible(video_widget_, FALSE);
    }
    if (poster_picture_ != nullptr) {
        gtk_widget_set_visible(poster_picture_, TRUE);
    }
    std::cerr << "[PowerMenuScene] video pipeline paused while idle\n";
}

void PowerMenuScene::destroy_media() noexcept {
    release_media();
    if (media_stream_ == nullptr) return;
    g_signal_handlers_disconnect_by_data(media_stream_, this);
    if (GTK_IS_MEDIA_FILE(media_stream_)) {
        gtk_media_file_clear(GTK_MEDIA_FILE(media_stream_));
    }
    media_source_loaded_ = false;
    g_clear_object(&media_stream_);
}

void PowerMenuScene::start_media_playback() noexcept {
    if (media_stream_ == nullptr || media_playback_started_) return;
    gtk_media_stream_play(media_stream_);
    media_playback_started_ = true;
}

void PowerMenuScene::pause_media_playback() noexcept {
    if (media_stream_ == nullptr) {
        media_playback_started_ = false;
        return;
    }
    gtk_media_stream_pause(media_stream_);
    media_playback_started_ = false;
}

void PowerMenuScene::sync_media_widgets() noexcept {
    if (poster_picture_ == nullptr || video_widget_ == nullptr) return;

    const bool stream_ready = media_stream_ != nullptr &&
        gtk_media_stream_is_prepared(media_stream_) &&
        gtk_media_stream_get_error(media_stream_) == nullptr;
    const bool live_video_allowed = live_video_committed_ && (
        state_.phase() == PowerMenuVideoPhase::Visible ||
        (state_.phase() == PowerMenuVideoPhase::Closing && ripple_fallback_)
    );
    const bool show_video = stream_ready && live_video_allowed;

    gtk_widget_set_visible(video_widget_, show_video);
    gtk_widget_set_visible(poster_picture_, !show_video);
}

bool PowerMenuScene::live_frame_ready() const noexcept {
    if (media_stream_ == nullptr ||
        !gtk_media_stream_is_prepared(media_stream_) ||
        gtk_media_stream_get_error(media_stream_) != nullptr ||
        gtk_media_stream_get_timestamp(media_stream_) <= 0) {
        return false;
    }

    GdkPaintable* image = gdk_paintable_get_current_image(
        GDK_PAINTABLE(media_stream_)
    );
    const bool ready = image != nullptr;
    g_clear_object(&image);
    return ready;
}

void PowerMenuScene::arm_live_handoff() noexcept {
    if (handoff_pending_ || handoff_active_ ||
        ripple_renderer_ == nullptr || !ripple_renderer_->active()) {
        return;
    }
    handoff_pending_ = true;
    handoff_started_us_ = 0;
    ripple_renderer_->set_opacity(1.0);
}

void PowerMenuScene::update_live_handoff(gint64 frame_time_us) noexcept {
    if (state_.phase() != PowerMenuVideoPhase::Visible ||
        ripple_renderer_ == nullptr || !ripple_renderer_->active()) {
        return;
    }

    if (handoff_pending_) {
        if (!live_frame_ready()) {
            // Keep the exact terminal shader frame on screen until the decoder
            // has actually advanced. Prepared alone only means metadata and a
            // bootstrap frame exist; switching at that point causes the pop.
            return;
        }
        live_video_committed_ = true;
        sync_media_widgets();
        handoff_pending_ = false;
        handoff_active_ = true;
        handoff_started_us_ = frame_time_us;
        std::cerr << "[PowerMenuRipple] live frame ready; beginning "
                  << (kLiveHandoffDurationUs / 1000)
                  << " ms dissolve\n";
    }

    if (!handoff_active_) return;

    const double linear = std::clamp(
        static_cast<double>(frame_time_us - handoff_started_us_) /
            static_cast<double>(kLiveHandoffDurationUs),
        0.0,
        1.0
    );
    const double eased = linear * linear * (3.0 - (2.0 * linear));
    ripple_renderer_->set_opacity(1.0 - eased);

    if (linear >= 1.0) {
        handoff_active_ = false;
        handoff_started_us_ = 0;
        finish_ripple();
        std::cerr << "[PowerMenuRipple] live-video handoff complete\n";
    }
}

void PowerMenuScene::cancel_live_handoff(bool keep_ripple) noexcept {
    handoff_pending_ = false;
    handoff_active_ = false;
    handoff_started_us_ = 0;
    if (keep_ripple && ripple_renderer_ != nullptr &&
        ripple_renderer_->active()) {
        ripple_renderer_->set_opacity(1.0);
    }
}

bool PowerMenuScene::handoff_needs_frame() const noexcept {
    return handoff_pending_ || handoff_active_;
}

void PowerMenuScene::handle_stream_notify(GtkMediaStream* stream) {
    if (const GError* error = gtk_media_stream_get_error(stream); error != nullptr) {
        error_message_ = std::string{"Unable to decode power-menu video: "} + error->message;
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
        if (stream == media_stream_) release_media();
        return;
    }
    if (stream == media_stream_ && gtk_media_stream_is_prepared(stream)) {
        sync_media_widgets();
    }
}

GdkPaintable* PowerMenuScene::transition_source() const noexcept {
    // Opening begins from the static first-frame poster so the click-to-light
    // response is deterministic. Closing captures the live stream instead, so
    // the collapsing scene matches whatever frame the user was actually seeing.
    if (state_.phase() == PowerMenuVideoPhase::Opening &&
        poster_texture_ != nullptr) {
        return GDK_PAINTABLE(poster_texture_);
    }
    if (media_stream_ != nullptr &&
        gtk_media_stream_is_prepared(media_stream_) &&
        gtk_media_stream_get_error(media_stream_) == nullptr) {
        return GDK_PAINTABLE(media_stream_);
    }
    if (poster_texture_ != nullptr) return GDK_PAINTABLE(poster_texture_);
    return nullptr;
}

bool PowerMenuScene::try_begin_ripple() {
    if (!ripple_pending_ || ripple_fallback_ || ripple_renderer_ == nullptr) {
        return false;
    }

    GdkPaintable* source = transition_source();
    if (source == nullptr) {
        ++ripple_attempts_;
    } else {
        std::string error;
        const bool opening = state_.phase() == PowerMenuVideoPhase::Opening;
        if (ripple_renderer_->begin(
                source,
                ripple_origin_x_,
                ripple_origin_y_,
                opening,
                &error)) {
            ripple_pending_ = false;
            ripple_attempts_ = 0;
            ripple_renderer_->update(state_.progress(), opening);
            if (!opening) pause_media_playback();
            return true;
        }
        ++ripple_attempts_;
        if (ripple_attempts_ == kMaximumRippleBootstrapAttempts) {
            std::cerr << "[PowerMenuRipple] unable to start shader: "
                      << error << '\n';
        }
    }

    if (ripple_attempts_ >= kMaximumRippleBootstrapAttempts) {
        ripple_pending_ = false;
        ripple_fallback_ = true;
        std::cerr << "[PowerMenuRipple] using opacity fallback for this transition\n";
    }
    return false;
}

void PowerMenuScene::finish_ripple() noexcept {
    if (ripple_renderer_ != nullptr) ripple_renderer_->finish();
}

gboolean PowerMenuScene::on_timer() {
    if (!state_.needs_frame() && !handoff_needs_frame()) {
        tick_callback_id_ = 0;
        last_frame_time_us_ = 0;
        return G_SOURCE_REMOVE;
    }

    const gint64 frame_time_us = g_get_monotonic_time();

    if (!state_.needs_frame()) {
        // The radial reveal has reached its terminal frame. Keep ticking only
        // long enough to wait for a genuinely live decoded frame and dissolve
        // the frozen GL texture into it without a one-frame swap.
        apply_frame();
        update_live_handoff(frame_time_us);
        if (handoff_needs_frame()) return G_SOURCE_CONTINUE;

        tick_callback_id_ = 0;
        last_frame_time_us_ = 0;
        return G_SOURCE_REMOVE;
    }

    // Mapping and compiling a cold GtkGLArea can take several compositor
    // frames. Hold the timeline at its exact endpoint until the first shader
    // frame exists; otherwise the reveal skips the ignition frames.
    if (ripple_pending_) try_begin_ripple();
    const bool waiting_for_ripple = !ripple_fallback_ && (
        ripple_pending_ ||
        (ripple_renderer_ != nullptr &&
         ripple_renderer_->active() &&
         !ripple_renderer_->frame_ready())
    );
    if (waiting_for_ripple) {
        if (ripple_renderer_ != nullptr && ripple_renderer_->active()) {
            ripple_renderer_->update(
                state_.progress(),
                state_.phase() == PowerMenuVideoPhase::Opening
            );
        }
        last_frame_time_us_ = frame_time_us;
        apply_frame();
        return G_SOURCE_CONTINUE;
    }

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

    if (state_.needs_frame() || handoff_needs_frame()) {
        return G_SOURCE_CONTINUE;
    }

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
    if (tick_callback_id_ != 0 ||
        (!state_.needs_frame() && !handoff_needs_frame())) return;
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
    if (widget_ == nullptr || media_layer_ == nullptr) return;

    const auto phase = state_.phase();
    if (phase == PowerMenuVideoPhase::Hidden) {
        finish_ripple();
        ripple_pending_ = false;
        ripple_fallback_ = false;
        ripple_attempts_ = 0;
        cancel_live_handoff(false);
        live_video_committed_ = false;
        release_media();
        // Keep the lazily-created GL area/program and decoded static poster as
        // one-time caches rather than constructing new fullscreen rendering
        // resources on every invocation. finish_ripple() has already deleted
        // the transition texture and hidden the non-auto-rendering GL area, so
        // the cached renderer does not schedule work while the menu is idle.
        gtk_widget_set_visible(media_layer_, FALSE);
        gtk_widget_set_opacity(media_layer_, 1.0);
        gtk_widget_set_opacity(widget_, 0.0);
        publish_visibility();
        return;
    }

    gtk_widget_set_opacity(widget_, 1.0);

    if (phase == PowerMenuVideoPhase::Visible) {
        ripple_pending_ = false;
        ripple_fallback_ = false;
        ripple_attempts_ = 0;

        gtk_widget_set_opacity(media_layer_, 1.0);
        gtk_widget_set_visible(media_layer_, TRUE);
        start_media_playback();

        if (ripple_renderer_ != nullptr && ripple_renderer_->active()) {
            // Keep the complete shader frame above the media picture. The live
            // stream is committed underneath only after its timestamp advances,
            // then GL dissolves away instead of disappearing in one frame.
            arm_live_handoff();
        } else {
            live_video_committed_ = true;
        }
        sync_media_widgets();
        publish_visibility();
        return;
    }

    const bool opening = phase == PowerMenuVideoPhase::Opening;
    if (ripple_renderer_ != nullptr && ripple_renderer_->active()) {
        ripple_renderer_->update(state_.progress(), opening);
    } else {
        if (!ripple_pending_ && !ripple_fallback_) {
            ripple_fallback_ = true;
            std::cerr << "[PowerMenuRipple] renderer stopped; continuing with opacity fallback\n";
        }
        if (ripple_pending_) try_begin_ripple();
    }

    const bool ripple_active = ripple_renderer_ != nullptr &&
        ripple_renderer_->active();
    const bool ripple_ready = ripple_active &&
        ripple_renderer_->frame_ready();

    if (ripple_ready) {
        // Remove the ordinary media tree entirely while the per-pixel-alpha
        // GL texture carries the travelling front across the desktop.
        gtk_widget_set_visible(media_layer_, FALSE);
        gtk_widget_set_opacity(media_layer_, 1.0);
    } else if (ripple_active || ripple_pending_) {
        // Opening maps as a genuinely empty transparent layer until GL has its
        // first frame. Closing keeps the already-visible scene in place until
        // an identical terminal shader frame is ready to replace it.
        gtk_widget_set_opacity(media_layer_, 1.0);
        gtk_widget_set_visible(media_layer_, !opening);
    } else if (ripple_fallback_) {
        gtk_widget_set_visible(media_layer_, TRUE);
        gtk_widget_set_opacity(media_layer_, state_.opacity());
        if (opening) {
            live_video_committed_ = false;
            pause_media_playback();
        } else {
            live_video_committed_ = true;
            start_media_playback();
        }
        sync_media_widgets();
    } else {
        gtk_widget_set_visible(media_layer_, !opening);
        gtk_widget_set_opacity(media_layer_, 1.0);
    }

    publish_visibility();
}

void PowerMenuScene::publish_visibility() {
    if (visibility_callback_) visibility_callback_(state_.controls_opacity());
}

} // namespace realmheart::ui::powermenu
