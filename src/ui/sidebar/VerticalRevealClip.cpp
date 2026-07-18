#include "ui/sidebar/VerticalRevealClip.hpp"

#include <algorithm>
#include <cmath>

struct _RealmheartVerticalRevealClip {
    GtkWidget parent_instance;

    GtkWidget* child = nullptr;
    double progress = 0.0;
    double animation_start_progress = 0.0;
    double animation_target_progress = 0.0;
    gint64 animation_start_us = 0;
    gint64 animation_duration_us = 0;
    guint opening_duration_ms = 0;
    guint closing_duration_ms = 0;
    guint initial_strip_px = 0;
    guint tick_id = 0;
};

G_DEFINE_TYPE(
    RealmheartVerticalRevealClip,
    realmheart_vertical_reveal_clip,
    GTK_TYPE_WIDGET
)

namespace {

enum SignalId {
    kConcealedSignal,
    kSignalCount,
};

guint signals[kSignalCount]{};

constexpr double kProgressEpsilon = 0.0001;

// The pill/strip and unfurl phases share this normalized timeline. Callers
// tune opening and closing independently: the current panel timings use a
// shorter 150 ms opening extension and a 100 ms closing extension.
//
// 0.000 -> 0.134: a tiny centred pill fades and grows into existence.
// 0.134 -> 0.282: that pill stretches horizontally into the leading strip.
// 0.282 -> 1.000: the real panel is uncovered from top to bottom.
constexpr double kPillArrivalEnd = 0.134;
constexpr double kStripFormationEnd = 0.282;
constexpr double kChildBlendStart = 0.230;

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

struct IntroGeometry {
    float x = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float radius = 0.0F;
    float opacity = 0.0F;
};

void set_progress(RealmheartVerticalRevealClip* self, double progress) {
    self->progress = clamp01(progress);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

float widget_width(RealmheartVerticalRevealClip* self) {
    return std::max(
        0.0F,
        static_cast<float>(gtk_widget_get_width(GTK_WIDGET(self)))
    );
}

float widget_height(RealmheartVerticalRevealClip* self) {
    return std::max(
        0.0F,
        static_cast<float>(gtk_widget_get_height(GTK_WIDGET(self)))
    );
}

float strip_height(RealmheartVerticalRevealClip* self) {
    return std::min(
        widget_height(self),
        static_cast<float>(self->initial_strip_px)
    );
}

IntroGeometry intro_geometry(RealmheartVerticalRevealClip* self) {
    IntroGeometry result;
    const float width = widget_width(self);
    const float target_height = strip_height(self);
    if (width <= 0.0F || target_height <= 0.0F || self->progress <= 0.0) {
        return result;
    }

    const float resting_pill_width = std::clamp(width * 0.22F, 52.0F, 74.0F);
    const float seed_width = resting_pill_width * 0.58F;
    const float seed_height = std::max(4.0F, target_height * 0.36F);
    const float resting_pill_height = std::max(8.0F, target_height * 0.68F);

    if (self->progress <= kPillArrivalEnd) {
        const double raw = phase_progress(self->progress, 0.0, kPillArrivalEnd);
        const double eased = ease_out_cubic(raw);
        result.width = seed_width
            + ((resting_pill_width - seed_width) * static_cast<float>(eased));
        result.height = seed_height
            + ((resting_pill_height - seed_height) * static_cast<float>(eased));
        result.opacity = static_cast<float>(smoothstep(raw));
        result.radius = result.height * 0.5F;
    } else {
        const double raw = phase_progress(
            self->progress,
            kPillArrivalEnd,
            kStripFormationEnd
        );
        const double eased = ease_in_out_cubic(raw);
        result.width = resting_pill_width
            + ((width - resting_pill_width) * static_cast<float>(eased));
        result.height = resting_pill_height
            + ((target_height - resting_pill_height) * static_cast<float>(eased));
        result.opacity = 1.0F;
        result.radius = result.height * 0.5F;
    }

    result.width = std::min(result.width, width);
    result.height = std::min(result.height, target_height);
    result.x = (width - result.width) * 0.5F;
    return result;
}

float revealed_height(RealmheartVerticalRevealClip* self) {
    if (self->progress <= 0.0) return 0.0F;
    if (self->progress < kStripFormationEnd) {
        return intro_geometry(self).height;
    }

    const float height = widget_height(self);
    const float strip = strip_height(self);
    const double raw = phase_progress(
        self->progress,
        kStripFormationEnd,
        1.0
    );
    const double eased = ease_out_cubic(raw);
    return strip + ((height - strip) * static_cast<float>(eased));
}

void append_rounded_color(
    GtkSnapshot* snapshot,
    const graphene_rect_t& rect,
    float radius,
    const GdkRGBA& color
) {
    if (rect.size.width <= 0.0F || rect.size.height <= 0.0F || color.alpha <= 0.0) {
        return;
    }

    GskRoundedRect rounded{};
    gsk_rounded_rect_init_from_rect(&rounded, &rect, std::max(0.0F, radius));
    gtk_snapshot_push_rounded_clip(snapshot, &rounded);
    gtk_snapshot_append_color(snapshot, &color, &rect);
    gtk_snapshot_pop(snapshot);
}

void snapshot_intro_shape(
    RealmheartVerticalRevealClip* self,
    GtkSnapshot* snapshot
) {
    const IntroGeometry geometry = intro_geometry(self);
    if (geometry.width <= 0.0F || geometry.height <= 0.0F || geometry.opacity <= 0.0F) {
        return;
    }

    // A deliberately simple synthetic shell prevents header text from being
    // exposed while the opening is still only pill-sized. It crossfades into
    // the real panel once the shape has almost completed its horizontal span.
    const double child_blend = smoothstep(phase_progress(
        self->progress,
        kChildBlendStart,
        kStripFormationEnd
    ));
    const float shell_opacity = geometry.opacity
        * static_cast<float>(1.0 - child_blend);
    GdkRGBA glow{0.45F, 0.92F, 0.82F, 0.13F * shell_opacity};
    GdkRGBA border{0.78F, 0.57F, 0.28F, 0.78F * shell_opacity};
    GdkRGBA surface{0.035F, 0.055F, 0.060F, 0.99F * shell_opacity};

    const graphene_rect_t glow_rect = GRAPHENE_RECT_INIT(
        geometry.x - 2.0F,
        0.0F,
        geometry.width + 4.0F,
        geometry.height
    );
    append_rounded_color(
        snapshot,
        glow_rect,
        geometry.height * 0.5F,
        glow
    );

    const graphene_rect_t outer_rect = GRAPHENE_RECT_INIT(
        geometry.x,
        0.0F,
        geometry.width,
        geometry.height
    );
    append_rounded_color(snapshot, outer_rect, geometry.radius, border);

    const float inset = std::min(1.0F, geometry.height * 0.16F);
    const graphene_rect_t inner_rect = GRAPHENE_RECT_INIT(
        geometry.x + inset,
        inset,
        std::max(0.0F, geometry.width - (2.0F * inset)),
        std::max(0.0F, geometry.height - (2.0F * inset))
    );
    append_rounded_color(
        snapshot,
        inner_rect,
        std::max(0.0F, geometry.radius - inset),
        surface
    );
}

void snapshot_child_intro_blend(
    RealmheartVerticalRevealClip* self,
    GtkWidget* widget,
    GtkSnapshot* snapshot
) {
    const double raw = phase_progress(
        self->progress,
        kChildBlendStart,
        kStripFormationEnd
    );
    if (raw <= 0.0) return;

    const IntroGeometry geometry = intro_geometry(self);
    if (geometry.width <= 0.0F || geometry.height <= 0.0F) return;

    const graphene_rect_t viewport = GRAPHENE_RECT_INIT(
        geometry.x,
        0.0F,
        geometry.width,
        geometry.height + 1.0F
    );
    GskRoundedRect rounded{};
    gsk_rounded_rect_init_from_rect(
        &rounded,
        &viewport,
        std::max(0.0F, geometry.radius)
    );

    gtk_snapshot_push_opacity(snapshot, static_cast<float>(smoothstep(raw)));
    gtk_snapshot_push_rounded_clip(snapshot, &rounded);
    gtk_widget_snapshot_child(widget, self->child, snapshot);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_pop(snapshot);
}

bool sample_animation(
    RealmheartVerticalRevealClip* self,
    gint64 now_us,
    bool emit_completion
) {
    if (self->animation_duration_us <= 0) {
        set_progress(self, self->animation_target_progress);
    } else {
        // Begin from a genuinely invisible first frame. The first visible
        // geometry is then produced by the frame clock rather than popping in
        // when the overlay itself becomes visible.
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

        // Opening uses linear timeline travel because each visual stage owns
        // its own easing curve. Closing uses a global ease-in-out so the lower
        // edge retreats gently before the final strip folds back into a pill.
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
    auto* self = REALMHEART_VERTICAL_REVEAL_CLIP(data);
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

void stop_animation(RealmheartVerticalRevealClip* self) {
    if (self->tick_id == 0) return;
    gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
    self->tick_id = 0;
}

void vertical_reveal_measure(
    GtkWidget* widget,
    GtkOrientation orientation,
    int for_size,
    int* minimum,
    int* natural,
    int* minimum_baseline,
    int* natural_baseline
) {
    auto* self = REALMHEART_VERTICAL_REVEAL_CLIP(widget);
    if (self->child == nullptr) {
        if (minimum != nullptr) *minimum = 0;
        if (natural != nullptr) *natural = 0;
        if (minimum_baseline != nullptr) *minimum_baseline = -1;
        if (natural_baseline != nullptr) *natural_baseline = -1;
        return;
    }

    // Progress never participates in layout. Text, borders and rows retain
    // their final geometry while only the snapshot viewport changes.
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

void vertical_reveal_size_allocate(
    GtkWidget* widget,
    int width,
    int height,
    int baseline
) {
    auto* self = REALMHEART_VERTICAL_REVEAL_CLIP(widget);
    if (self->child == nullptr) return;
    gtk_widget_allocate(self->child, width, height, baseline, nullptr);
}

void vertical_reveal_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    auto* self = REALMHEART_VERTICAL_REVEAL_CLIP(widget);
    if (self->child == nullptr || self->progress <= kProgressEpsilon) return;

    const float width = widget_width(self);
    const float visible_height = revealed_height(self);
    if (width <= 0.0F || visible_height <= 0.0F) return;

    if (self->progress >= 1.0 - kProgressEpsilon) {
        gtk_widget_snapshot_child(widget, self->child, snapshot);
        return;
    }

    if (self->progress < kStripFormationEnd) {
        snapshot_intro_shape(self, snapshot);
        snapshot_child_intro_blend(self, widget, snapshot);
        return;
    }

    const graphene_rect_t viewport = GRAPHENE_RECT_INIT(
        0.0F,
        0.0F,
        width,
        visible_height
    );
    gtk_snapshot_push_clip(snapshot, &viewport);
    gtk_widget_snapshot_child(widget, self->child, snapshot);
    gtk_snapshot_pop(snapshot);
}

gboolean vertical_reveal_contains(GtkWidget* widget, double x, double y) {
    auto* self = REALMHEART_VERTICAL_REVEAL_CLIP(widget);
    if (self->progress <= kProgressEpsilon) return FALSE;

    const double visible_height = static_cast<double>(revealed_height(self));
    if (self->progress < kStripFormationEnd) {
        const IntroGeometry geometry = intro_geometry(self);
        return x >= geometry.x
            && y >= 0.0
            && x < geometry.x + geometry.width
            && y < visible_height;
    }

    const double width = static_cast<double>(gtk_widget_get_width(widget));
    return x >= 0.0 && y >= 0.0 && x < width && y < visible_height;
}

void vertical_reveal_dispose(GObject* object) {
    auto* self = REALMHEART_VERTICAL_REVEAL_CLIP(object);
    stop_animation(self);

    if (self->child != nullptr) {
        gtk_widget_unparent(self->child);
        self->child = nullptr;
    }

    G_OBJECT_CLASS(realmheart_vertical_reveal_clip_parent_class)->dispose(object);
}

} // namespace

static void realmheart_vertical_reveal_clip_class_init(
    RealmheartVerticalRevealClipClass* klass
) {
    auto* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = vertical_reveal_dispose;

    auto* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = vertical_reveal_measure;
    widget_class->size_allocate = vertical_reveal_size_allocate;
    widget_class->snapshot = vertical_reveal_snapshot;
    widget_class->contains = vertical_reveal_contains;

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

static void realmheart_vertical_reveal_clip_init(
    RealmheartVerticalRevealClip* self
) {
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget* realmheart_vertical_reveal_clip_new(
    GtkWidget* child,
    guint opening_duration_ms,
    guint closing_duration_ms,
    guint initial_strip_px
) {
    g_return_val_if_fail(GTK_IS_WIDGET(child), nullptr);

    auto* self = REALMHEART_VERTICAL_REVEAL_CLIP(
        g_object_new(REALMHEART_TYPE_VERTICAL_REVEAL_CLIP, nullptr)
    );
    self->child = child;
    self->opening_duration_ms = opening_duration_ms;
    self->closing_duration_ms = closing_duration_ms;
    self->initial_strip_px = initial_strip_px;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

void realmheart_vertical_reveal_clip_set_revealed(
    RealmheartVerticalRevealClip* self,
    gboolean revealed
) {
    g_return_if_fail(REALMHEART_IS_VERTICAL_REVEAL_CLIP(self));

    const gint64 now_us = g_get_monotonic_time();
    if (self->tick_id != 0) {
        static_cast<void>(sample_animation(self, now_us, false));
    }

    const double target = revealed ? 1.0 : 0.0;
    if (std::abs(target - self->progress) <= kProgressEpsilon) {
        stop_animation(self);
        self->animation_target_progress = target;
        set_progress(self, target);
        if (!revealed) g_signal_emit(self, signals[kConcealedSignal], 0);
        return;
    }

    self->animation_start_progress = self->progress;
    self->animation_target_progress = target;
    self->animation_start_us = self->tick_id == 0 ? 0 : now_us;
    const guint base_duration = revealed
        ? self->opening_duration_ms
        : self->closing_duration_ms;
    self->animation_duration_us = std::max<gint64>(
        1,
        static_cast<gint64>(std::llround(
            static_cast<double>(base_duration) * 1000.0
                * std::abs(target - self->progress)
        ))
    );

    if (self->tick_id == 0) {
        self->tick_id = gtk_widget_add_tick_callback(
            GTK_WIDGET(self),
            animation_tick,
            self,
            nullptr
        );
    }
}

void realmheart_vertical_reveal_clip_set_revealed_immediately(
    RealmheartVerticalRevealClip* self,
    gboolean revealed
) {
    g_return_if_fail(REALMHEART_IS_VERTICAL_REVEAL_CLIP(self));

    stop_animation(self);
    const double target = revealed ? 1.0 : 0.0;
    self->animation_start_progress = target;
    self->animation_target_progress = target;
    self->animation_start_us = 0;
    self->animation_duration_us = 0;
    set_progress(self, target);
}

gboolean realmheart_vertical_reveal_clip_is_concealed(
    RealmheartVerticalRevealClip* self
) {
    g_return_val_if_fail(REALMHEART_IS_VERTICAL_REVEAL_CLIP(self), TRUE);
    return self->progress <= kProgressEpsilon;
}
