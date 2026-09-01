#include "ui/OSDOverlay.hpp"

#include "ui/LayerSurface.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace {

struct RealmheartOSDRevealClip {
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

struct RealmheartOSDRevealClipClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(
    RealmheartOSDRevealClip,
    realmheart_osd_reveal_clip,
    GTK_TYPE_WIDGET
)

enum SignalId {
    kConcealedSignal,
    kSignalCount,
};

guint signals[kSignalCount]{};

constexpr double kProgressEpsilon = 0.0001;
constexpr double kPillArrivalEnd = 0.25;
constexpr double kStripFormationEnd = 0.78;
constexpr double kChildFadeStart = 0.70;

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

void set_progress(RealmheartOSDRevealClip* self, double progress) {
    self->progress = clamp01(progress);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

RevealGeometry reveal_geometry(RealmheartOSDRevealClip* self) {
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

    const float seed_width = std::clamp(full_width * 0.12F, 40.0F, 48.0F);
    const float pill_width = std::clamp(full_width * 0.18F, 56.0F, 70.0F);
    const float seed_height = std::clamp(full_height * 0.14F, 7.0F, 10.0F);
    const float pill_height = std::clamp(full_height * 0.24F, 12.0F, 16.0F);

    if (self->progress <= kPillArrivalEnd) {
        const double raw = phase_progress(self->progress, 0.0, kPillArrivalEnd);
        const double eased = ease_out_cubic(raw);
        geometry.width = seed_width
            + ((pill_width - seed_width) * static_cast<float>(eased));
        geometry.height = seed_height
            + ((pill_height - seed_height) * static_cast<float>(eased));
        geometry.radius = geometry.height * 0.5F;
        geometry.opacity = static_cast<float>(smoothstep(raw));
    } else {
        const double raw = phase_progress(
            self->progress,
            kPillArrivalEnd,
            kStripFormationEnd
        );
        const double eased = ease_in_out_cubic(raw);
        geometry.width = pill_width
            + ((full_width - pill_width) * static_cast<float>(eased));
        geometry.height = pill_height
            + ((full_height - pill_height) * static_cast<float>(eased));
        const float pill_radius = pill_height * 0.5F;
        geometry.radius = pill_radius
            + ((18.0F - pill_radius) * static_cast<float>(eased));
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
    RealmheartOSDRevealClip* self,
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

    GdkRGBA border{0.78F, 0.57F, 0.28F, 0.90F * shell_opacity};
    GdkRGBA inner_border{0.50F, 0.72F, 0.93F, 0.32F * shell_opacity};
    GdkRGBA surface{0.035F, 0.050F, 0.058F, 0.99F * shell_opacity};

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
    RealmheartOSDRevealClip* self,
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
        const double eased = opening ? raw : ease_in_out_cubic(raw);
        set_progress(
            self,
            self->animation_start_progress
                + ((self->animation_target_progress
                    - self->animation_start_progress) * eased)
        );
        if (raw < 1.0) return false;
    }

    set_progress(self, self->animation_target_progress);
    if (emit_completion && self->animation_target_progress <= kProgressEpsilon) {
        g_signal_emit(self, signals[kConcealedSignal], 0);
    }
    return true;
}

gboolean animation_tick(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer data
) {
    auto* self = static_cast<RealmheartOSDRevealClip*>(data);
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

void stop_animation(RealmheartOSDRevealClip* self) {
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
    auto* self = static_cast<RealmheartOSDRevealClip*>(
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
    auto* self = static_cast<RealmheartOSDRevealClip*>(
        reinterpret_cast<void*>(widget)
    );
    if (self->child == nullptr) return;
    gtk_widget_allocate(self->child, width, height, baseline, nullptr);
}

void reveal_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    auto* self = static_cast<RealmheartOSDRevealClip*>(
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
    auto* self = static_cast<RealmheartOSDRevealClip*>(
        reinterpret_cast<void*>(object)
    );
    stop_animation(self);
    if (self->child != nullptr) {
        gtk_widget_unparent(self->child);
        self->child = nullptr;
    }
    G_OBJECT_CLASS(realmheart_osd_reveal_clip_parent_class)->dispose(object);
}

void realmheart_osd_reveal_clip_class_init(RealmheartOSDRevealClipClass* klass) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = reveal_dispose;

    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = reveal_measure;
    widget_class->size_allocate = reveal_size_allocate;
    widget_class->snapshot = reveal_snapshot;
    gtk_widget_class_set_css_name(widget_class, "realmheart-osd-reveal");

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

void realmheart_osd_reveal_clip_init(RealmheartOSDRevealClip* self) {
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_can_target(GTK_WIDGET(self), FALSE);
}

GtkWidget* osd_reveal_clip_new(
    GtkWidget* child,
    guint opening_duration_ms,
    guint closing_duration_ms
) {
    auto* self = static_cast<RealmheartOSDRevealClip*>(g_object_new(
        realmheart_osd_reveal_clip_get_type(),
        nullptr
    ));
    self->child = child;
    self->opening_duration_ms = opening_duration_ms;
    self->closing_duration_ms = closing_duration_ms;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

void osd_reveal_clip_set_revealed(
    RealmheartOSDRevealClip* self,
    bool revealed
) {
    const double target = revealed ? 1.0 : 0.0;
    if (std::abs(target - self->progress) <= kProgressEpsilon && self->tick_id == 0) {
        if (!revealed) g_signal_emit(self, signals[kConcealedSignal], 0);
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

void osd_reveal_clip_set_revealed_immediately(
    RealmheartOSDRevealClip* self,
    bool revealed
) {
    stop_animation(self);
    self->animation_start_progress = revealed ? 1.0 : 0.0;
    self->animation_target_progress = self->animation_start_progress;
    self->animation_start_us = 0;
    self->animation_duration_us = 0;
    set_progress(self, self->animation_target_progress);
}

RealmheartOSDRevealClip* as_osd_reveal(GtkWidget* widget) {
    return static_cast<RealmheartOSDRevealClip*>(
        reinterpret_cast<void*>(widget)
    );
}

} // namespace

namespace realmheart::ui {

namespace {

constexpr int kOSDWidth = 360;
constexpr int kOSDHeight = 58;
constexpr guint kOpeningDurationMs = 300;
constexpr guint kClosingDurationMs = 270;
constexpr guint kDismissDelayMs = 1050;

std::string volume_icon_for(double percent) {
    if (percent <= 0.5) return "Realmheart-Icons/speaker-0.svg";
    if (percent < 45.0) return "Realmheart-Icons/speaker-1.svg";
    return "Realmheart-Icons/speaker-2.svg";
}

std::string percent_text(double percent) {
    return std::to_string(static_cast<int>(std::lround(percent))) + "%";
}

} // namespace

OSDOverlay::OSDOverlay(
    GtkApplication* app,
    std::function<void(bool)> visibility_changed,
    int monitor_index
)
    : app_(app),
      monitor_index_(monitor_index),
      visibility_changed_(std::move(visibility_changed)) {
    window_ = GTK_WIDGET(gtk_application_window_new(app_));
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window_), kOSDWidth, kOSDHeight);
    gtk_widget_add_css_class(window_, "realmheart-osd-window");
    gtk_widget_set_can_target(window_, FALSE);

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(card, kOSDWidth, kOSDHeight);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(card, "realmheart-osd-card");
    gtk_widget_set_can_target(card, FALSE);

    icon_ = std::make_unique<bar::widgets::ThemedSvgIcon>(
        "Realmheart-Icons/speaker-2.svg",
        24
    );
    icon_->add_css_class("realmheart-osd-icon");
    gtk_widget_set_can_target(icon_->widget(), FALSE);
    gtk_box_append(GTK_BOX(card), icon_->widget());

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_hexpand(body, TRUE);
    gtk_widget_set_valign(body, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(body, FALSE);

    GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_widget_set_can_target(heading, FALSE);

    title_label_ = gtk_label_new("Volume");
    gtk_widget_set_hexpand(title_label_, TRUE);
    gtk_widget_set_halign(title_label_, GTK_ALIGN_START);
    gtk_widget_add_css_class(title_label_, "realmheart-osd-title");
    gtk_widget_set_can_target(title_label_, FALSE);
    gtk_box_append(GTK_BOX(heading), title_label_);

    value_label_ = gtk_label_new("0%");
    gtk_widget_set_halign(value_label_, GTK_ALIGN_END);
    gtk_widget_add_css_class(value_label_, "realmheart-osd-value");
    gtk_widget_set_can_target(value_label_, FALSE);
    gtk_box_append(GTK_BOX(heading), value_label_);

    progress_ = gtk_progress_bar_new();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_), 0.0);
    gtk_widget_set_hexpand(progress_, TRUE);
    gtk_widget_add_css_class(progress_, "realmheart-osd-progress");
    gtk_widget_set_can_target(progress_, FALSE);

    gtk_box_append(GTK_BOX(body), heading);
    gtk_box_append(GTK_BOX(body), progress_);
    gtk_box_append(GTK_BOX(card), body);

    reveal_ = osd_reveal_clip_new(
        card,
        kOpeningDurationMs,
        kClosingDurationMs
    );
    gtk_widget_set_size_request(reveal_, kOSDWidth, kOSDHeight);
    gtk_window_set_child(GTK_WINDOW(window_), reveal_);

    g_signal_connect(
        reveal_,
        "concealed",
        G_CALLBACK(+[](RealmheartOSDRevealClip*, gpointer data) {
            auto* self = static_cast<OSDOverlay*>(data);
            self->stop_progress_animation();
            gtk_widget_set_visible(self->window_, FALSE);
            if (self->visibility_changed_) self->visibility_changed_(false);
        }),
        this
    );

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-osd";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::None;
    spec.anchor_top = true;
    spec.margin_top = 28;
    spec.monitor_index = monitor_index_;
    apply_layer_surface(GTK_WINDOW(window_), spec);

    osd_reveal_clip_set_revealed_immediately(as_osd_reveal(reveal_), false);
    gtk_widget_set_visible(window_, FALSE);
}

OSDOverlay::~OSDOverlay() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    stop_progress_animation();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void OSDOverlay::show_volume(double percent) {
    const double clamped = std::clamp(percent, 0.0, 100.0);
    show_value("Volume", volume_icon_for(clamped), clamped);
}

void OSDOverlay::show_brightness(double percent) {
    show_value(
        "Brightness",
        "Realmheart-Icons/brightness.svg",
        std::clamp(percent, 0.0, 100.0)
    );
}

void OSDOverlay::show_value(
    const std::string& label,
    const std::string& icon_path,
    double percent
) {
    const double clamped = std::clamp(percent, 0.0, 100.0);
    gtk_label_set_text(GTK_LABEL(title_label_), label.c_str());
    static_cast<void>(icon_->set_icon(icon_path));

    const bool was_visible = gtk_widget_get_visible(window_);
    if (!was_visible) {
        displayed_percent_ = 0.0;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_), 0.0);
        gtk_label_set_text(GTK_LABEL(value_label_), "0%");
        osd_reveal_clip_set_revealed_immediately(as_osd_reveal(reveal_), false);
        gtk_window_present(GTK_WINDOW(window_));
        if (visibility_changed_) visibility_changed_(true);
        osd_reveal_clip_set_revealed(as_osd_reveal(reveal_), true);
        animate_progress_to(clamped, 180);
    } else {
        osd_reveal_clip_set_revealed(as_osd_reveal(reveal_), true);
        animate_progress_to(clamped, 120);
    }

    schedule_dismiss();
}

void OSDOverlay::animate_progress_to(double percent, guint duration_ms) {
    const double clamped = std::clamp(percent, 0.0, 100.0);
    stop_progress_animation();

    progress_animation_start_ = displayed_percent_;
    progress_animation_target_ = clamped;
    progress_animation_start_us_ = 0;
    progress_animation_duration_us_ = static_cast<gint64>(duration_ms) * 1000;

    if (std::abs(progress_animation_target_ - progress_animation_start_) < 0.01 ||
        progress_animation_duration_us_ <= 0) {
        displayed_percent_ = progress_animation_target_;
        gtk_progress_bar_set_fraction(
            GTK_PROGRESS_BAR(progress_),
            displayed_percent_ / 100.0
        );
        gtk_label_set_text(
            GTK_LABEL(value_label_),
            percent_text(displayed_percent_).c_str()
        );
        return;
    }

    progress_tick_id_ = gtk_widget_add_tick_callback(
        progress_,
        &OSDOverlay::progress_tick,
        this,
        nullptr
    );
}

void OSDOverlay::stop_progress_animation() {
    if (progress_tick_id_ == 0 || progress_ == nullptr) return;
    gtk_widget_remove_tick_callback(progress_, progress_tick_id_);
    progress_tick_id_ = 0;
}

gboolean OSDOverlay::progress_tick(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer data
) {
    auto* self = static_cast<OSDOverlay*>(data);
    const gint64 now_us = gdk_frame_clock_get_frame_time(frame_clock);
    if (self->progress_animation_start_us_ == 0) {
        self->progress_animation_start_us_ = now_us;
    }

    const double elapsed = static_cast<double>(
        std::max<gint64>(0, now_us - self->progress_animation_start_us_)
    );
    const double raw = self->progress_animation_duration_us_ <= 0
        ? 1.0
        : clamp01(
            elapsed / static_cast<double>(self->progress_animation_duration_us_)
        );
    const double eased = ease_out_cubic(raw);
    self->displayed_percent_ = self->progress_animation_start_
        + ((self->progress_animation_target_ - self->progress_animation_start_) * eased);

    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(self->progress_),
        self->displayed_percent_ / 100.0
    );
    gtk_label_set_text(
        GTK_LABEL(self->value_label_),
        percent_text(self->displayed_percent_).c_str()
    );

    if (raw < 1.0) return G_SOURCE_CONTINUE;
    self->displayed_percent_ = self->progress_animation_target_;
    self->progress_tick_id_ = 0;
    return G_SOURCE_REMOVE;
}

void OSDOverlay::schedule_dismiss() {
    if (timeout_id_ != 0) g_source_remove(timeout_id_);
    timeout_id_ = g_timeout_add(kDismissDelayMs, &OSDOverlay::dismiss_timeout, this);
}

gboolean OSDOverlay::dismiss_timeout(gpointer data) {
    auto* self = static_cast<OSDOverlay*>(data);
    self->timeout_id_ = 0;
    self->dismiss();
    return G_SOURCE_REMOVE;
}

void OSDOverlay::dismiss() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    if (window_ == nullptr || !gtk_widget_get_visible(window_)) return;
    osd_reveal_clip_set_revealed(as_osd_reveal(reveal_), false);
}

} // namespace realmheart::ui
