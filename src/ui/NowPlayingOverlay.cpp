#include "ui/NowPlayingOverlay.hpp"

#include "ui/LayerSurface.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

struct RealmheartNowPlayingReveal {
    GtkWidget parent_instance;

    GtkWidget* child = nullptr;
    double progress = 0.0;
    double animation_start_progress = 0.0;
    double animation_target_progress = 0.0;
    gint64 animation_start_us = 0;
    gint64 animation_duration_us = 0;
    guint opening_duration_ms = 0;
    guint closing_duration_ms = 0;
    guint tick_id = 0;
};

struct RealmheartNowPlayingRevealClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(
    RealmheartNowPlayingReveal,
    realmheart_now_playing_reveal,
    GTK_TYPE_WIDGET
)

enum SignalId {
    kRevealedSignal,
    kConcealedSignal,
    kSignalCount,
};

guint signals[kSignalCount]{};

constexpr double kProgressEpsilon = 0.0001;
constexpr double kLineArrivalEnd = 0.20;
constexpr double kShellFormationEnd = 0.82;
constexpr double kChildFadeStart = 0.68;

inline double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

inline double phase_progress(double value, double start, double end) {
    if (end <= start) return value >= end ? 1.0 : 0.0;
    return clamp01((value - start) / (end - start));
}

inline double smoothstep(double value) {
    const double clamped = clamp01(value);
    return clamped * clamped * (3.0 - (2.0 * clamped));
}

inline double ease_out_cubic(double value) {
    const double clamped = clamp01(value);
    const double inverse = 1.0 - clamped;
    return 1.0 - (inverse * inverse * inverse);
}

inline double ease_in_out_cubic(double value) {
    const double clamped = clamp01(value);
    if (clamped < 0.5) return 4.0 * clamped * clamped * clamped;
    const double inverse = (-2.0 * clamped) + 2.0;
    return 1.0 - ((inverse * inverse * inverse) / 2.0);
}

struct RevealGeometry {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float radius = 0.0F;
    float opacity = 0.0F;
};

void set_progress(RealmheartNowPlayingReveal* self, double progress) {
    self->progress = clamp01(progress);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

RevealGeometry reveal_geometry(RealmheartNowPlayingReveal* self) {
    RevealGeometry geometry;
    const float full_width = static_cast<float>(
        std::max(0, gtk_widget_get_width(GTK_WIDGET(self)))
    );
    const float full_height = static_cast<float>(
        std::max(0, gtk_widget_get_height(GTK_WIDGET(self)))
    );
    if (full_width <= 0.0F || full_height <= 0.0F || self->progress <= 0.0) {
        return geometry;
    }

    constexpr float kLineHeight = 2.0F;

    // The Now Playing animation is deliberately not pill-like: its horizontal
    // footprint is present from the first frame to the last. Only the vertical
    // reveal changes, so the 2px seed reads as a true micro-strip rather than a
    // small capsule growing in both axes.
    geometry.width = full_width;

    if (self->progress <= kLineArrivalEnd) {
        const double raw = phase_progress(self->progress, 0.0, kLineArrivalEnd);
        geometry.height = kLineHeight;
        geometry.radius = kLineHeight * 0.5F;
        geometry.opacity = static_cast<float>(smoothstep(raw));
    } else {
        const double raw = phase_progress(
            self->progress,
            kLineArrivalEnd,
            kShellFormationEnd
        );
        const double eased = ease_in_out_cubic(raw);
        geometry.height = kLineHeight
            + ((full_height - kLineHeight) * static_cast<float>(eased));
        geometry.radius = (kLineHeight * 0.5F)
            + ((16.0F - (kLineHeight * 0.5F)) * static_cast<float>(eased));
        geometry.opacity = 1.0F;
    }

    geometry.width = std::min(geometry.width, full_width);
    geometry.height = std::min(geometry.height, full_height);
    geometry.x = (full_width - geometry.width) * 0.5F;
    geometry.y = (full_height - geometry.height) * 0.5F;
    return geometry;
}

void append_rounded_color(
    GtkSnapshot* snapshot,
    const graphene_rect_t& rect,
    float radius,
    const GdkRGBA& color
) {
    if (rect.size.width <= 0.0F || rect.size.height <= 0.0F || color.alpha <= 0.0F) {
        return;
    }

    GskRoundedRect rounded{};
    gsk_rounded_rect_init_from_rect(&rounded, &rect, std::max(0.0F, radius));
    gtk_snapshot_push_rounded_clip(snapshot, &rounded);
    gtk_snapshot_append_color(snapshot, &color, &rect);
    gtk_snapshot_pop(snapshot);
}

void snapshot_synthetic_shell(
    RealmheartNowPlayingReveal* self,
    GtkSnapshot* snapshot,
    const RevealGeometry& geometry
) {
    const double child_blend = smoothstep(phase_progress(
        self->progress,
        kChildFadeStart,
        1.0
    ));
    const float shell_opacity = geometry.opacity
        * static_cast<float>(1.0 - child_blend);

    const GdkRGBA border{0.78F, 0.57F, 0.28F, 0.88F * shell_opacity};
    const GdkRGBA inner_border{0.50F, 0.72F, 0.93F, 0.30F * shell_opacity};
    const GdkRGBA surface{0.035F, 0.050F, 0.058F, 0.99F * shell_opacity};

    const graphene_rect_t outer = GRAPHENE_RECT_INIT(
        geometry.x,
        geometry.y,
        geometry.width,
        geometry.height
    );
    append_rounded_color(snapshot, outer, geometry.radius, border);

    const float outer_inset = std::min(1.0F, geometry.height * 0.12F);
    const graphene_rect_t middle = GRAPHENE_RECT_INIT(
        geometry.x + outer_inset,
        geometry.y + outer_inset,
        std::max(0.0F, geometry.width - (2.0F * outer_inset)),
        std::max(0.0F, geometry.height - (2.0F * outer_inset))
    );
    append_rounded_color(
        snapshot,
        middle,
        std::max(0.0F, geometry.radius - outer_inset),
        inner_border
    );

    const float inner_inset = outer_inset + 1.0F;
    const graphene_rect_t inner = GRAPHENE_RECT_INIT(
        geometry.x + inner_inset,
        geometry.y + inner_inset,
        std::max(0.0F, geometry.width - (2.0F * inner_inset)),
        std::max(0.0F, geometry.height - (2.0F * inner_inset))
    );
    append_rounded_color(
        snapshot,
        inner,
        std::max(0.0F, geometry.radius - inner_inset),
        surface
    );
}

bool sample_animation(
    RealmheartNowPlayingReveal* self,
    gint64 now_us,
    bool emit_completion
) {
    if (self->animation_duration_us <= 0) {
        set_progress(self, self->animation_target_progress);
    } else {
        if (self->animation_start_us == 0) {
            self->animation_start_us = now_us;
            set_progress(self, self->animation_start_progress);
            return false;
        }

        const double elapsed = static_cast<double>(
            std::max<gint64>(0, now_us - self->animation_start_us)
        );
        const double raw = clamp01(
            elapsed / static_cast<double>(self->animation_duration_us)
        );
        const bool opening =
            self->animation_target_progress > self->animation_start_progress;
        const double eased = opening ? ease_out_cubic(raw) : ease_in_out_cubic(raw);
        set_progress(
            self,
            self->animation_start_progress
                + ((self->animation_target_progress
                    - self->animation_start_progress) * eased)
        );
        if (raw < 1.0) return false;
    }

    set_progress(self, self->animation_target_progress);
    if (emit_completion) {
        if (self->animation_target_progress >= 1.0 - kProgressEpsilon) {
            g_signal_emit(self, signals[kRevealedSignal], 0);
        } else if (self->animation_target_progress <= kProgressEpsilon) {
            g_signal_emit(self, signals[kConcealedSignal], 0);
        }
    }
    return true;
}

gboolean animation_tick(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer data
) {
    auto* self = static_cast<RealmheartNowPlayingReveal*>(data);
    if (!sample_animation(
            self,
            gdk_frame_clock_get_frame_time(frame_clock),
            true
        )) {
        return G_SOURCE_CONTINUE;
    }

    self->tick_id = 0;
    return G_SOURCE_REMOVE;
}

void stop_animation(RealmheartNowPlayingReveal* self) {
    if (self->tick_id == 0) return;
    gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
    self->tick_id = 0;
}

void reveal_measure(
    GtkWidget* widget,
    GtkOrientation orientation,
    int for_size,
    int* minimum,
    int* natural,
    int* minimum_baseline,
    int* natural_baseline
) {
    auto* self = static_cast<RealmheartNowPlayingReveal*>(
        reinterpret_cast<void*>(widget)
    );
    if (self->child == nullptr) {
        if (minimum != nullptr) *minimum = 0;
        if (natural != nullptr) *natural = 0;
        if (minimum_baseline != nullptr) *minimum_baseline = -1;
        if (natural_baseline != nullptr) *natural_baseline = -1;
        return;
    }

    gtk_widget_measure(
        self->child,
        orientation,
        for_size,
        minimum,
        natural,
        minimum_baseline,
        natural_baseline
    );
}

void reveal_size_allocate(
    GtkWidget* widget,
    int width,
    int height,
    int baseline
) {
    auto* self = static_cast<RealmheartNowPlayingReveal*>(
        reinterpret_cast<void*>(widget)
    );
    if (self->child == nullptr) return;
    gtk_widget_allocate(self->child, width, height, baseline, nullptr);
}

void reveal_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    auto* self = static_cast<RealmheartNowPlayingReveal*>(
        reinterpret_cast<void*>(widget)
    );
    if (self->child == nullptr || self->progress <= kProgressEpsilon) return;

    if (self->progress >= 1.0 - kProgressEpsilon) {
        gtk_widget_snapshot_child(widget, self->child, snapshot);
        return;
    }

    const RevealGeometry geometry = reveal_geometry(self);
    if (geometry.width <= 0.0F || geometry.height <= 0.0F) return;

    snapshot_synthetic_shell(self, snapshot, geometry);

    const double child_opacity = smoothstep(phase_progress(
        self->progress,
        kChildFadeStart,
        1.0
    ));
    if (child_opacity <= 0.0) return;

    const graphene_rect_t clip = GRAPHENE_RECT_INIT(
        geometry.x,
        geometry.y,
        geometry.width,
        geometry.height
    );
    GskRoundedRect rounded{};
    gsk_rounded_rect_init_from_rect(&rounded, &clip, geometry.radius);

    gtk_snapshot_push_opacity(snapshot, static_cast<float>(child_opacity));
    gtk_snapshot_push_rounded_clip(snapshot, &rounded);
    gtk_widget_snapshot_child(widget, self->child, snapshot);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_pop(snapshot);
}

void reveal_dispose(GObject* object) {
    auto* self = static_cast<RealmheartNowPlayingReveal*>(
        reinterpret_cast<void*>(object)
    );
    stop_animation(self);
    if (self->child != nullptr) {
        gtk_widget_unparent(self->child);
        self->child = nullptr;
    }
    G_OBJECT_CLASS(realmheart_now_playing_reveal_parent_class)->dispose(object);
}

void realmheart_now_playing_reveal_class_init(
    RealmheartNowPlayingRevealClass* klass
) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = reveal_dispose;

    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = reveal_measure;
    widget_class->size_allocate = reveal_size_allocate;
    widget_class->snapshot = reveal_snapshot;
    gtk_widget_class_set_css_name(widget_class, "realmheart-now-playing-reveal");

    signals[kRevealedSignal] = g_signal_new(
        "revealed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        nullptr,
        nullptr,
        nullptr,
        G_TYPE_NONE,
        0
    );
    signals[kConcealedSignal] = g_signal_new(
        "concealed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0,
        nullptr,
        nullptr,
        nullptr,
        G_TYPE_NONE,
        0
    );
}

void realmheart_now_playing_reveal_init(RealmheartNowPlayingReveal* self) {
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_can_target(GTK_WIDGET(self), FALSE);
}

GtkWidget* now_playing_reveal_new(
    GtkWidget* child,
    guint opening_duration_ms,
    guint closing_duration_ms
) {
    auto* self = static_cast<RealmheartNowPlayingReveal*>(g_object_new(
        realmheart_now_playing_reveal_get_type(),
        nullptr
    ));
    self->child = child;
    self->opening_duration_ms = opening_duration_ms;
    self->closing_duration_ms = closing_duration_ms;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

void now_playing_reveal_set_revealed(
    RealmheartNowPlayingReveal* self,
    bool revealed
) {
    const double target = revealed ? 1.0 : 0.0;
    if (std::abs(target - self->progress) <= kProgressEpsilon && self->tick_id == 0) {
        return;
    }

    stop_animation(self);
    self->animation_start_progress = self->progress;
    self->animation_target_progress = target;
    self->animation_start_us = 0;

    const guint base_duration = revealed
        ? self->opening_duration_ms
        : self->closing_duration_ms;
    self->animation_duration_us = static_cast<gint64>(
        static_cast<double>(base_duration) * 1000.0
        * std::abs(self->animation_target_progress - self->animation_start_progress)
    );
    self->tick_id = gtk_widget_add_tick_callback(
        GTK_WIDGET(self),
        animation_tick,
        self,
        nullptr
    );
}

void now_playing_reveal_set_revealed_immediately(
    RealmheartNowPlayingReveal* self,
    bool revealed
) {
    stop_animation(self);
    self->animation_start_progress = revealed ? 1.0 : 0.0;
    self->animation_target_progress = self->animation_start_progress;
    self->animation_start_us = 0;
    self->animation_duration_us = 0;
    set_progress(self, self->animation_target_progress);
}

bool now_playing_reveal_is_fully_revealed(
    const RealmheartNowPlayingReveal* self
) {
    return self->progress >= 1.0 - kProgressEpsilon && self->tick_id == 0;
}

RealmheartNowPlayingReveal* as_now_playing_reveal(GtkWidget* widget) {
    return static_cast<RealmheartNowPlayingReveal*>(
        reinterpret_cast<void*>(widget)
    );
}

} // namespace

namespace realmheart::ui {
namespace {

constexpr int kNowPlayingWidth = 300;
constexpr int kNowPlayingHeight = 50;
constexpr int kNormalTopMargin = 28;
constexpr int kSystemOSDTopMargin = 96;
constexpr guint kOpeningDurationMs = 340;
constexpr guint kClosingDurationMs = 280;
constexpr guint kVisibleDurationMs = 2500;
constexpr guint kMarginMoveDurationMs = 160;

std::string fallback_artist(const std::string& artist) {
    return artist.empty() ? "Unknown artist" : artist;
}

} // namespace

NowPlayingOverlay::NowPlayingOverlay(GtkApplication* app) : app_(app) {
    window_ = GTK_WIDGET(gtk_application_window_new(app_));
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_default_size(
        GTK_WINDOW(window_),
        kNowPlayingWidth,
        kNowPlayingHeight
    );
    gtk_widget_add_css_class(window_, "realmheart-now-playing-window");
    gtk_widget_set_can_target(window_, FALSE);

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_size_request(card, kNowPlayingWidth, kNowPlayingHeight);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(card, "realmheart-now-playing-card");
    gtk_widget_set_can_target(card, FALSE);

    icon_ = std::make_unique<bar::widgets::ThemedSvgIcon>(
        "Realmheart-Icons/media.svg",
        20
    );
    icon_->add_css_class("realmheart-now-playing-icon");
    gtk_widget_set_can_target(icon_->widget(), FALSE);
    gtk_box_append(GTK_BOX(card), icon_->widget());

    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(text, FALSE);

    title_label_ = gtk_label_new("Unknown track");
    gtk_widget_set_hexpand(title_label_, TRUE);
    gtk_widget_set_halign(title_label_, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(title_label_), 0.0F);
    gtk_label_set_single_line_mode(GTK_LABEL(title_label_), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(title_label_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title_label_), 28);
    gtk_widget_add_css_class(title_label_, "realmheart-now-playing-title");
    gtk_widget_set_can_target(title_label_, FALSE);

    artist_label_ = gtk_label_new("Unknown artist");
    gtk_widget_set_hexpand(artist_label_, TRUE);
    gtk_widget_set_halign(artist_label_, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(artist_label_), 0.0F);
    gtk_label_set_single_line_mode(GTK_LABEL(artist_label_), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(artist_label_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(artist_label_), 31);
    gtk_widget_add_css_class(artist_label_, "realmheart-now-playing-artist");
    gtk_widget_set_can_target(artist_label_, FALSE);

    gtk_box_append(GTK_BOX(text), title_label_);
    gtk_box_append(GTK_BOX(text), artist_label_);
    gtk_box_append(GTK_BOX(card), text);

    reveal_ = now_playing_reveal_new(
        card,
        kOpeningDurationMs,
        kClosingDurationMs
    );
    gtk_widget_set_size_request(
        reveal_,
        kNowPlayingWidth,
        kNowPlayingHeight
    );
    gtk_window_set_child(GTK_WINDOW(window_), reveal_);

    g_signal_connect(
        reveal_,
        "revealed",
        G_CALLBACK(+[](RealmheartNowPlayingReveal*, gpointer data) {
            static_cast<NowPlayingOverlay*>(data)->schedule_dismiss();
        }),
        this
    );
    g_signal_connect(
        reveal_,
        "concealed",
        G_CALLBACK(+[](RealmheartNowPlayingReveal*, gpointer data) {
            auto* self = static_cast<NowPlayingOverlay*>(data);
            self->stop_margin_animation();
            gtk_widget_set_visible(self->window_, FALSE);
        }),
        this
    );

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-now-playing";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::None;
    spec.anchor_top = true;
    spec.margin_top = kNormalTopMargin;
    apply_layer_surface(GTK_WINDOW(window_), spec);

    current_top_margin_ = kNormalTopMargin;
    margin_animation_start_ = kNormalTopMargin;
    margin_animation_target_ = kNormalTopMargin;

    now_playing_reveal_set_revealed_immediately(
        as_now_playing_reveal(reveal_),
        false
    );
    gtk_widget_set_visible(window_, FALSE);
}

NowPlayingOverlay::~NowPlayingOverlay() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    stop_margin_animation();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void NowPlayingOverlay::show(
    const std::string& title,
    const std::string& artist
) {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }

    const std::string visible_title = title.empty() ? "Unknown track" : title;
    gtk_label_set_text(GTK_LABEL(title_label_), visible_title.c_str());
    const std::string visible_artist = fallback_artist(artist);
    gtk_label_set_text(GTK_LABEL(artist_label_), visible_artist.c_str());

    const int requested_margin = system_osd_visible_
        ? kSystemOSDTopMargin
        : kNormalTopMargin;
    const bool was_visible = gtk_widget_get_visible(window_);
    if (!was_visible) {
        stop_margin_animation();
        current_top_margin_ = requested_margin;
        margin_animation_start_ = requested_margin;
        margin_animation_target_ = requested_margin;
        gtk_layer_set_margin(
            GTK_WINDOW(window_),
            GTK_LAYER_SHELL_EDGE_TOP,
            requested_margin
        );
    } else {
        animate_top_margin_to(requested_margin);
    }

    auto* reveal = as_now_playing_reveal(reveal_);
    if (!was_visible) {
        now_playing_reveal_set_revealed_immediately(reveal, false);
        gtk_window_present(GTK_WINDOW(window_));
    }

    if (now_playing_reveal_is_fully_revealed(reveal)) {
        schedule_dismiss();
    } else {
        now_playing_reveal_set_revealed(reveal, true);
    }
}

void NowPlayingOverlay::schedule_dismiss() {
    if (timeout_id_ != 0) g_source_remove(timeout_id_);
    timeout_id_ = g_timeout_add(
        kVisibleDurationMs,
        &NowPlayingOverlay::dismiss_timeout,
        this
    );
}

gboolean NowPlayingOverlay::dismiss_timeout(gpointer data) {
    auto* self = static_cast<NowPlayingOverlay*>(data);
    self->timeout_id_ = 0;
    self->dismiss();
    return G_SOURCE_REMOVE;
}

void NowPlayingOverlay::dismiss() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    if (window_ == nullptr || !gtk_widget_get_visible(window_)) return;
    now_playing_reveal_set_revealed(as_now_playing_reveal(reveal_), false);
}

void NowPlayingOverlay::set_system_osd_visible(bool visible) {
    system_osd_visible_ = visible;
    const int target = visible ? kSystemOSDTopMargin : kNormalTopMargin;
    if (window_ == nullptr || !gtk_widget_get_visible(window_)) {
        stop_margin_animation();
        current_top_margin_ = target;
        margin_animation_start_ = target;
        margin_animation_target_ = target;
        gtk_layer_set_margin(
            GTK_WINDOW(window_),
            GTK_LAYER_SHELL_EDGE_TOP,
            target
        );
        return;
    }
    animate_top_margin_to(target);
}

void NowPlayingOverlay::animate_top_margin_to(int target_margin) {
    if (target_margin == current_top_margin_) return;
    stop_margin_animation();
    margin_animation_start_ = current_top_margin_;
    margin_animation_target_ = target_margin;
    margin_animation_start_us_ = 0;
    margin_tick_id_ = gtk_widget_add_tick_callback(
        window_,
        &NowPlayingOverlay::margin_tick,
        this,
        nullptr
    );
}

void NowPlayingOverlay::stop_margin_animation() {
    if (margin_tick_id_ == 0 || window_ == nullptr) return;
    gtk_widget_remove_tick_callback(window_, margin_tick_id_);
    margin_tick_id_ = 0;
}

gboolean NowPlayingOverlay::margin_tick(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer data
) {
    auto* self = static_cast<NowPlayingOverlay*>(data);
    const gint64 now_us = gdk_frame_clock_get_frame_time(frame_clock);
    if (self->margin_animation_start_us_ == 0) {
        self->margin_animation_start_us_ = now_us;
    }

    const double elapsed = static_cast<double>(
        std::max<gint64>(0, now_us - self->margin_animation_start_us_)
    );
    const double raw = clamp01(
        elapsed / (static_cast<double>(kMarginMoveDurationMs) * 1000.0)
    );
    const double eased = ease_in_out_cubic(raw);
    const double interpolated = static_cast<double>(self->margin_animation_start_)
        + (static_cast<double>(
            self->margin_animation_target_ - self->margin_animation_start_
        ) * eased);
    self->current_top_margin_ = static_cast<int>(std::lround(interpolated));
    gtk_layer_set_margin(
        GTK_WINDOW(self->window_),
        GTK_LAYER_SHELL_EDGE_TOP,
        self->current_top_margin_
    );

    if (raw < 1.0) return G_SOURCE_CONTINUE;
    self->current_top_margin_ = self->margin_animation_target_;
    self->margin_tick_id_ = 0;
    return G_SOURCE_REMOVE;
}

} // namespace realmheart::ui
