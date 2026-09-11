#include "ui/events/EventRevealClip.hpp"

#include <algorithm>
#include <cmath>

struct _RealmheartEventRevealClip {
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
    guint64 animation_generation = 0;
    GdkTexture* animation_texture = nullptr;
};

G_DEFINE_TYPE(
    RealmheartEventRevealClip,
    realmheart_event_reveal_clip,
    GTK_TYPE_WIDGET
)

namespace {

enum SignalId {
    kRevealedSignal,
    kConcealedSignal,
    kSignalCount,
};

guint signals[kSignalCount]{};
constexpr double kProgressEpsilon = 0.0001;

inline double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

inline double smoothstep(double value) {
    const double clamped = clamp01(value);
    return clamped * clamped * (3.0 - (2.0 * clamped));
}

void set_progress(RealmheartEventRevealClip* self, double progress) {
    self->progress = clamp01(progress);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void clear_animation_texture(RealmheartEventRevealClip* self) {
    if (self->animation_texture == nullptr) return;
    g_object_unref(self->animation_texture);
    self->animation_texture = nullptr;
}

bool capture_animation_texture(RealmheartEventRevealClip* self) {
    clear_animation_texture(self);
    if (self->child == nullptr) return false;

    const int width = gtk_widget_get_width(GTK_WIDGET(self));
    const int height = gtk_widget_get_height(GTK_WIDGET(self));
    if (width <= 0 || height <= 0) return false;

    GtkNative* native = gtk_widget_get_native(GTK_WIDGET(self));
    if (native == nullptr) return false;
    GskRenderer* renderer = gtk_native_get_renderer(native);
    if (renderer == nullptr || !gsk_renderer_is_realized(renderer)) return false;

    // A GskRenderNode is immutable, but it is still a scene graph containing
    // independently-rendered subnodes. On some Wayland/layer-shell paths, a
    // moving clip over a tall composite node can expose a one-frame seam where
    // the lower portion appears detached from the upper portion. Rasterize the
    // complete card to one texture first, then animate only that texture. This
    // gives every transition frame a single visual source and completely
    // decouples the animation from GtkScrolledWindow/viewport subtrees.
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gtk_widget_snapshot_child(GTK_WIDGET(self), self->child, snapshot);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    if (node == nullptr) return false;

    const graphene_rect_t viewport = GRAPHENE_RECT_INIT(
        0.0F,
        0.0F,
        static_cast<float>(width),
        static_cast<float>(height)
    );
    self->animation_texture = gsk_renderer_render_texture(renderer, node, &viewport);
    gsk_render_node_unref(node);
    return self->animation_texture != nullptr;
}

float visible_height(RealmheartEventRevealClip* self) {
    const float height = static_cast<float>(
        std::max(0, gtk_widget_get_height(GTK_WIDGET(self)))
    );
    return height * static_cast<float>(smoothstep(self->progress));
}

void stop_animation(RealmheartEventRevealClip* self) {
    if (self->tick_id != 0) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    clear_animation_texture(self);
}

bool sample_animation(
    RealmheartEventRevealClip* self,
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
    if (emit_completion) {
        if (self->animation_target_progress <= kProgressEpsilon) {
            g_signal_emit(self, signals[kConcealedSignal], 0);
        } else if (self->animation_target_progress >= 1.0 - kProgressEpsilon) {
            g_signal_emit(self, signals[kRevealedSignal], 0);
        }
    }
    return true;
}

gboolean animation_tick(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer data
) {
    auto* self = REALMHEART_EVENT_REVEAL_CLIP(data);

    // Opening is started immediately after gtk_window_present(). The first
    // idle can run before the native renderer is ready, so a texture capture
    // attempted in set_revealed() may legitimately fail. Retry on the first
    // frame(s) while progress is still at the starting edge; do not begin the
    // animation clock until we have the atomic texture.
    if (self->animation_texture == nullptr &&
        !capture_animation_texture(self) &&
        self->animation_start_us == 0) {
        return G_SOURCE_CONTINUE;
    }

    if (!sample_animation(
            self,
            gdk_frame_clock_get_frame_time(frame_clock),
            false
        )) {
        return G_SOURCE_CONTINUE;
    }

    // The completion signal may hide the layer surface or immediately start
    // the next reveal. Mark this tick as finished *before* invoking external
    // code so observers see a genuinely settled state. In particular:
    //
    //  - is_concealed() must return true from the concealed handler; and
    //  - if the handler starts a new reveal, its new tick id must not be
    //    overwritten when this old callback returns.
    const guint64 completed_generation = self->animation_generation;
    self->tick_id = 0;
    if (self->animation_target_progress <= kProgressEpsilon) {
        g_signal_emit(self, signals[kConcealedSignal], 0);
    } else if (self->animation_target_progress >= 1.0 - kProgressEpsilon) {
        g_signal_emit(self, signals[kRevealedSignal], 0);
    }

    // A completion handler is allowed to immediately start the opposite
    // transition. In that case capture_animation_texture() has installed a new
    // texture and we must leave it alone. Otherwise release the frozen frame
    // now that the live child can safely render again.
    if (self->tick_id == 0 &&
        self->animation_generation == completed_generation) {
        clear_animation_texture(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
    return G_SOURCE_REMOVE;
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
    auto* self = REALMHEART_EVENT_REVEAL_CLIP(widget);
    if (self->child == nullptr) {
        if (minimum != nullptr) *minimum = 0;
        if (natural != nullptr) *natural = 0;
        if (minimum_baseline != nullptr) *minimum_baseline = -1;
        if (natural_baseline != nullptr) *natural_baseline = -1;
        return;
    }

    // Progress deliberately does not participate in measurement. The layer
    // surface keeps its final geometry for the entire animation.
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
    auto* self = REALMHEART_EVENT_REVEAL_CLIP(widget);
    if (self->child == nullptr) return;
    gtk_widget_allocate(self->child, width, height, baseline, nullptr);
}

void reveal_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    auto* self = REALMHEART_EVENT_REVEAL_CLIP(widget);
    if (self->child == nullptr || self->progress <= kProgressEpsilon) return;

    const bool animating = self->tick_id != 0 && self->animation_texture != nullptr;
    if (self->progress >= 1.0 - kProgressEpsilon && !animating) {
        gtk_widget_snapshot_child(widget, self->child, snapshot);
        return;
    }

    const float width = static_cast<float>(std::max(0, gtk_widget_get_width(widget)));
    const float height = visible_height(self);
    if (width <= 0.0F || height <= 0.0F) return;

    const graphene_rect_t clip = GRAPHENE_RECT_INIT(0.0F, 0.0F, width, height);
    gtk_snapshot_push_clip(snapshot, &clip);
    if (animating) {
        const graphene_rect_t bounds = GRAPHENE_RECT_INIT(
            0.0F,
            0.0F,
            width,
            static_cast<float>(std::max(0, gtk_widget_get_height(widget)))
        );
        gtk_snapshot_append_texture(snapshot, self->animation_texture, &bounds);
    } else {
        gtk_widget_snapshot_child(widget, self->child, snapshot);
    }
    gtk_snapshot_pop(snapshot);
}

gboolean reveal_contains(GtkWidget* widget, double x, double y) {
    auto* self = REALMHEART_EVENT_REVEAL_CLIP(widget);
    if (self->progress <= kProgressEpsilon) return FALSE;
    const double width = static_cast<double>(gtk_widget_get_width(widget));
    const double height = static_cast<double>(visible_height(self));
    return x >= 0.0 && y >= 0.0 && x < width && y < height;
}

void reveal_dispose(GObject* object) {
    auto* self = REALMHEART_EVENT_REVEAL_CLIP(object);
    stop_animation(self);
    clear_animation_texture(self);
    if (self->child != nullptr) {
        gtk_widget_unparent(self->child);
        self->child = nullptr;
    }
    G_OBJECT_CLASS(realmheart_event_reveal_clip_parent_class)->dispose(object);
}

} // namespace

static void realmheart_event_reveal_clip_class_init(
    RealmheartEventRevealClipClass* klass
) {
    auto* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = reveal_dispose;

    auto* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = reveal_measure;
    widget_class->size_allocate = reveal_size_allocate;
    widget_class->snapshot = reveal_snapshot;
    widget_class->contains = reveal_contains;

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

static void realmheart_event_reveal_clip_init(RealmheartEventRevealClip* self) {
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget* realmheart_event_reveal_clip_new(
    GtkWidget* child,
    guint opening_duration_ms,
    guint closing_duration_ms
) {
    g_return_val_if_fail(GTK_IS_WIDGET(child), nullptr);

    auto* self = REALMHEART_EVENT_REVEAL_CLIP(
        g_object_new(REALMHEART_TYPE_EVENT_REVEAL_CLIP, nullptr)
    );
    self->child = child;
    self->opening_duration_ms = opening_duration_ms;
    self->closing_duration_ms = closing_duration_ms;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

void realmheart_event_reveal_clip_set_revealed(
    RealmheartEventRevealClip* self,
    gboolean revealed
) {
    g_return_if_fail(REALMHEART_IS_EVENT_REVEAL_CLIP(self));

    const gint64 now_us = g_get_monotonic_time();
    if (self->tick_id != 0) {
        static_cast<void>(sample_animation(self, now_us, false));
    }

    const double target = revealed ? 1.0 : 0.0;
    if (std::abs(target - self->progress) <= kProgressEpsilon) {
        stop_animation(self);
        self->animation_target_progress = target;
        set_progress(self, target);
        if (revealed) {
            g_signal_emit(self, signals[kRevealedSignal], 0);
        } else {
            g_signal_emit(self, signals[kConcealedSignal], 0);
        }
        return;
    }

    stop_animation(self);
    ++self->animation_generation;
    static_cast<void>(capture_animation_texture(self));
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

void realmheart_event_reveal_clip_set_revealed_immediately(
    RealmheartEventRevealClip* self,
    gboolean revealed
) {
    g_return_if_fail(REALMHEART_IS_EVENT_REVEAL_CLIP(self));
    stop_animation(self);
    self->animation_start_progress = revealed ? 1.0 : 0.0;
    self->animation_target_progress = self->animation_start_progress;
    self->animation_start_us = 0;
    self->animation_duration_us = 0;
    set_progress(self, self->animation_target_progress);
}

gboolean realmheart_event_reveal_clip_get_revealed(
    RealmheartEventRevealClip* self
) {
    g_return_val_if_fail(REALMHEART_IS_EVENT_REVEAL_CLIP(self), FALSE);
    return self->animation_target_progress >= 1.0 - kProgressEpsilon;
}

gboolean realmheart_event_reveal_clip_is_concealed(
    RealmheartEventRevealClip* self
) {
    g_return_val_if_fail(REALMHEART_IS_EVENT_REVEAL_CLIP(self), TRUE);
    return self->progress <= kProgressEpsilon && self->tick_id == 0;
}
