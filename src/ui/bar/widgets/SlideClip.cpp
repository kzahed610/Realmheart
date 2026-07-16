#include "ui/bar/widgets/SlideClip.hpp"

#include <algorithm>
#include <cmath>

struct _RealmheartSlideClip {
    GtkWidget parent_instance;

    GtkWidget* child = nullptr;
    double progress = 0.0;
    double animation_start_progress = 0.0;
    double animation_target_progress = 0.0;
    gint64 animation_start_us = 0;
    gint64 animation_duration_us = 0;
    guint base_duration_ms = 0;
    guint leading_edge_travel_px = 0;
    guint tick_id = 0;
};

G_DEFINE_TYPE(RealmheartSlideClip, realmheart_slide_clip, GTK_TYPE_WIDGET)

namespace {

enum SignalId {
    kConcealedSignal,
    kSignalCount,
};

guint signals[kSignalCount]{};

constexpr double kProgressEpsilon = 0.0001;

inline double smoothstep(double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    return clamped * clamped * (3.0 - (2.0 * clamped));
}

void set_progress(RealmheartSlideClip* self, double progress) {
    self->progress = std::clamp(progress, 0.0, 1.0);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

bool sample_animation(
    RealmheartSlideClip* self,
    gint64 now_us,
    bool emit_completion
) {
    if (self->animation_duration_us <= 0) {
        set_progress(self, self->animation_target_progress);
    } else {
        // Anchor a newly-started animation to the first frame clock sample,
        // not to the input event that requested it. Mapping a layer surface or
        // waiting for the compositor can otherwise consume several milliseconds
        // before the first drawable frame and make the panel visibly jump ahead.
        if (self->animation_start_us == 0) {
            self->animation_start_us = now_us;
            set_progress(self, self->animation_start_progress);
            return false;
        }
        const double elapsed = static_cast<double>(
            std::max<gint64>(0, now_us - self->animation_start_us)
        );
        const double raw = std::clamp(
            elapsed / static_cast<double>(self->animation_duration_us),
            0.0,
            1.0
        );
        // A symmetric zero-velocity curve prevents the first compositor
        // frame from exposing a large chunk of the panel at once. The previous
        // ease-out curve had high initial velocity, which read as a tiny pop
        // before the otherwise-correct horizontal slide.
        const double eased = smoothstep(raw);
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
    auto* self = REALMHEART_SLIDE_CLIP(data);
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

void stop_animation(RealmheartSlideClip* self) {
    if (self->tick_id == 0) return;
    gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
    self->tick_id = 0;
}

void slide_clip_measure(
    GtkWidget* widget,
    GtkOrientation orientation,
    int for_size,
    int* minimum,
    int* natural,
    int* minimum_baseline,
    int* natural_baseline
) {
    auto* self = REALMHEART_SLIDE_CLIP(widget);
    if (self->child == nullptr) {
        if (minimum != nullptr) *minimum = 0;
        if (natural != nullptr) *natural = 0;
        if (minimum_baseline != nullptr) *minimum_baseline = -1;
        if (natural_baseline != nullptr) *natural_baseline = -1;
        return;
    }

    // Always report the complete child's natural geometry. Animation progress
    // must never participate in measurement or allocation.
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

void slide_clip_size_allocate(
    GtkWidget* widget,
    int width,
    int height,
    int baseline
) {
    auto* self = REALMHEART_SLIDE_CLIP(widget);
    if (self->child == nullptr) return;

    // The child receives its full final allocation on every frame. Only the
    // snapshot below is clipped, so Cairo paths never see a temporary width.
    gtk_widget_allocate(self->child, width, height, baseline, nullptr);
}

void slide_clip_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    auto* self = REALMHEART_SLIDE_CLIP(widget);
    if (self->child == nullptr || self->progress <= kProgressEpsilon) return;

    if (self->progress >= 1.0 - kProgressEpsilon) {
        gtk_widget_snapshot_child(widget, self->child, snapshot);
        return;
    }

    const float width = static_cast<float>(gtk_widget_get_width(widget));
    const float height = static_cast<float>(gtk_widget_get_height(widget));
    const bool leading_edge_reveal = self->leading_edge_travel_px > 0;
    const float viewport_width = leading_edge_reveal
        ? width * static_cast<float>(self->progress)
        : width;
    const float travel = leading_edge_reveal
        ? std::min(width, static_cast<float>(self->leading_edge_travel_px))
        : width;
    const graphene_rect_t viewport = GRAPHENE_RECT_INIT(
        0.0F,
        0.0F,
        viewport_width,
        height
    );
    const graphene_point_t translation = GRAPHENE_POINT_INIT(
        -travel * static_cast<float>(1.0 - self->progress),
        0.0F
    );

    // Standard mode translates the whole shell by its full width. The optional
    // leading-edge mode is media-specific: the viewport expands from the
    // taskbar edge while the already-final shell travels only a short distance.
    // That keeps the media panel's decorative screen-hug right shoulder hidden
    // until the panel has actually emerged, instead of letting the gold curve
    // lead the animation. Geometry still never participates in allocation.
    gtk_snapshot_save(snapshot);
    gtk_snapshot_push_clip(snapshot, &viewport);
    gtk_snapshot_translate(snapshot, &translation);
    gtk_widget_snapshot_child(widget, self->child, snapshot);
    gtk_snapshot_pop(snapshot);
    gtk_snapshot_restore(snapshot);
}

void slide_clip_dispose(GObject* object) {
    auto* self = REALMHEART_SLIDE_CLIP(object);
    stop_animation(self);

    if (self->child != nullptr) {
        gtk_widget_unparent(self->child);
        self->child = nullptr;
    }

    G_OBJECT_CLASS(realmheart_slide_clip_parent_class)->dispose(object);
}

} // namespace

static void realmheart_slide_clip_class_init(RealmheartSlideClipClass* klass) {
    auto* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = slide_clip_dispose;

    auto* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = slide_clip_measure;
    widget_class->size_allocate = slide_clip_size_allocate;
    widget_class->snapshot = slide_clip_snapshot;

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

static void realmheart_slide_clip_init(RealmheartSlideClip* self) {
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget* realmheart_slide_clip_new(GtkWidget* child, guint duration_ms) {
    g_return_val_if_fail(GTK_IS_WIDGET(child), nullptr);

    auto* self = REALMHEART_SLIDE_CLIP(
        g_object_new(REALMHEART_TYPE_SLIDE_CLIP, nullptr)
    );
    self->child = child;
    self->base_duration_ms = duration_ms;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}


void realmheart_slide_clip_set_leading_edge_reveal(
    RealmheartSlideClip* self,
    guint travel_px
) {
    g_return_if_fail(REALMHEART_IS_SLIDE_CLIP(self));
    self->leading_edge_travel_px = travel_px;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void realmheart_slide_clip_set_revealed(
    RealmheartSlideClip* self,
    gboolean revealed
) {
    g_return_if_fail(REALMHEART_IS_SLIDE_CLIP(self));

    const gint64 now_us = g_get_monotonic_time();
    if (self->tick_id != 0) {
        static_cast<void>(sample_animation(self, now_us, false));
    }

    const double target = revealed ? 1.0 : 0.0;
    if (std::abs(target - self->progress) <= kProgressEpsilon) {
        stop_animation(self);
        self->animation_target_progress = target;
        set_progress(self, target);
        return;
    }

    self->animation_start_progress = self->progress;
    self->animation_target_progress = target;
    // A fresh reveal/conceal begins on the first actual frame-clock sample.
    // During a mid-animation reversal, keep the current clock anchor so the
    // direction change remains immediate and continuous.
    self->animation_start_us = self->tick_id == 0 ? 0 : now_us;
    self->animation_duration_us = std::max<gint64>(
        1,
        static_cast<gint64>(std::llround(
            static_cast<double>(self->base_duration_ms) * 1000.0
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

void realmheart_slide_clip_set_revealed_immediately(
    RealmheartSlideClip* self,
    gboolean revealed
) {
    g_return_if_fail(REALMHEART_IS_SLIDE_CLIP(self));

    stop_animation(self);
    const double target = revealed ? 1.0 : 0.0;
    self->animation_start_progress = target;
    self->animation_target_progress = target;
    self->animation_start_us = 0;
    self->animation_duration_us = 0;
    set_progress(self, target);
}

gboolean realmheart_slide_clip_is_concealed(RealmheartSlideClip* self) {
    g_return_val_if_fail(REALMHEART_IS_SLIDE_CLIP(self), TRUE);
    return self->progress <= kProgressEpsilon;
}
