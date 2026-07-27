#include "ui/NotificationToast.hpp"

#include "ui/LayerSurface.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

struct RealmheartNotificationReveal {
    GtkWidget parent_instance;

    GtkWidget* child = nullptr;
    GtkWidget* close_button = nullptr;
    double progress = 0.0;
    double animation_start_progress = 0.0;
    double animation_target_progress = 0.0;
    gint64 animation_start_us = 0;
    gint64 animation_duration_us = 0;
    guint opening_duration_ms = 0;
    guint closing_duration_ms = 0;
    guint tick_id = 0;
};

struct RealmheartNotificationRevealClass {
    GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(
    RealmheartNotificationReveal,
    realmheart_notification_reveal,
    GTK_TYPE_WIDGET
)

enum SignalId {
    kRevealedSignal,
    kConcealedSignal,
    kSignalCount,
};

guint signals[kSignalCount]{};

constexpr double kProgressEpsilon = 0.0001;
constexpr double kPillSlideEnd = 0.34;
constexpr double kStripFormationEnd = 0.82;
constexpr double kChildFadeStart = 0.66;
constexpr double kCloseButtonTargetStart = 0.88;

// The toast reveal must measure as a fixed-size surface. GtkWidget size
// requests are minimums, so forwarding the card's natural height allows a
// long notification body to enlarge the layer surface.
constexpr int kFixedToastWidth = 396;
constexpr int kFixedToastHeight = 96;

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

RevealGeometry reveal_geometry(RealmheartNotificationReveal* self) {
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

    const float pill_width = std::clamp(full_width * 0.14F, 48.0F, 58.0F);
    const float pill_height = std::clamp(full_height * 0.16F, 12.0F, 16.0F);

    if (self->progress <= kPillSlideEnd) {
        const double raw = phase_progress(self->progress, 0.0, kPillSlideEnd);
        const double eased = ease_out_cubic(raw);
        const float start_right = full_width + pill_width;
        const float end_right = full_width;
        const float right = start_right
            + ((end_right - start_right) * static_cast<float>(eased));

        geometry.width = pill_width;
        geometry.height = pill_height;
        geometry.x = right - pill_width;
        geometry.y = (full_height - pill_height) * 0.5F;
        geometry.radius = pill_height * 0.5F;
        geometry.opacity = static_cast<float>(smoothstep(raw));
        return geometry;
    }

    const double raw = phase_progress(
        self->progress,
        kPillSlideEnd,
        kStripFormationEnd
    );
    const double eased = ease_in_out_cubic(raw);
    geometry.width = pill_width
        + ((full_width - pill_width) * static_cast<float>(eased));
    geometry.height = pill_height
        + ((full_height - pill_height) * static_cast<float>(eased));
    geometry.x = full_width - geometry.width;
    geometry.y = (full_height - geometry.height) * 0.5F;
    const float pill_radius = pill_height * 0.5F;
    geometry.radius = pill_radius
        + ((18.0F - pill_radius) * static_cast<float>(eased));
    geometry.opacity = 1.0F;
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
    RealmheartNotificationReveal* self,
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

    GdkRGBA border{0.80F, 0.59F, 0.29F, 0.90F * shell_opacity};
    GdkRGBA inner_border{0.49F, 0.74F, 0.94F, 0.30F * shell_opacity};
    GdkRGBA surface{0.032F, 0.048F, 0.056F, 0.99F * shell_opacity};

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

void set_progress(RealmheartNotificationReveal* self, double progress) {
    self->progress = clamp01(progress);
    if (self->close_button != nullptr) {
        const bool close_is_targetable =
            self->progress >= kCloseButtonTargetStart &&
            self->animation_target_progress > 0.0;
        gtk_widget_set_can_target(self->close_button, close_is_targetable);
    }
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

bool sample_animation(
    RealmheartNotificationReveal* self,
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
    auto* self = static_cast<RealmheartNotificationReveal*>(data);
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

void stop_animation(RealmheartNotificationReveal* self) {
    if (self->tick_id == 0) return;
    gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
    self->tick_id = 0;
}

void reveal_measure(
    GtkWidget*,
    GtkOrientation orientation,
    int,
    int* minimum,
    int* natural,
    int* minimum_baseline,
    int* natural_baseline
) {
    const int fixed_size = orientation == GTK_ORIENTATION_HORIZONTAL
        ? kFixedToastWidth
        : kFixedToastHeight;

    if (minimum != nullptr) *minimum = fixed_size;
    if (natural != nullptr) *natural = fixed_size;
    if (minimum_baseline != nullptr) *minimum_baseline = -1;
    if (natural_baseline != nullptr) *natural_baseline = -1;
}

void reveal_size_allocate(
    GtkWidget* widget,
    int width,
    int height,
    int baseline
) {
    auto* self = static_cast<RealmheartNotificationReveal*>(
        reinterpret_cast<void*>(widget)
    );
    if (self->child == nullptr) return;
    gtk_widget_allocate(self->child, width, height, baseline, nullptr);
}

void reveal_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    auto* self = static_cast<RealmheartNotificationReveal*>(
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

gboolean reveal_contains(GtkWidget* widget, double x, double y) {
    auto* self = static_cast<RealmheartNotificationReveal*>(
        reinterpret_cast<void*>(widget)
    );
    if (self->progress <= kProgressEpsilon) return FALSE;

    const RevealGeometry geometry = reveal_geometry(self);
    return x >= static_cast<double>(geometry.x) &&
        x <= static_cast<double>(geometry.x + geometry.width) &&
        y >= static_cast<double>(geometry.y) &&
        y <= static_cast<double>(geometry.y + geometry.height);
}

void reveal_dispose(GObject* object) {
    auto* self = static_cast<RealmheartNotificationReveal*>(
        reinterpret_cast<void*>(object)
    );
    stop_animation(self);
    if (self->child != nullptr) {
        gtk_widget_unparent(self->child);
        self->child = nullptr;
    }
    self->close_button = nullptr;
    G_OBJECT_CLASS(realmheart_notification_reveal_parent_class)->dispose(object);
}

void realmheart_notification_reveal_class_init(
    RealmheartNotificationRevealClass* klass
) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = reveal_dispose;

    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = reveal_measure;
    widget_class->size_allocate = reveal_size_allocate;
    widget_class->snapshot = reveal_snapshot;
    widget_class->contains = reveal_contains;
    gtk_widget_class_set_css_name(widget_class, "realmheart-notification-reveal");

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

void realmheart_notification_reveal_init(RealmheartNotificationReveal* self) {
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget* notification_reveal_new(
    GtkWidget* child,
    GtkWidget* close_button,
    guint opening_duration_ms,
    guint closing_duration_ms
) {
    auto* self = static_cast<RealmheartNotificationReveal*>(g_object_new(
        realmheart_notification_reveal_get_type(),
        nullptr
    ));
    self->child = child;
    self->close_button = close_button;
    self->opening_duration_ms = opening_duration_ms;
    self->closing_duration_ms = closing_duration_ms;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    gtk_widget_set_can_target(close_button, FALSE);
    return GTK_WIDGET(self);
}

void notification_reveal_set_revealed(
    RealmheartNotificationReveal* self,
    bool revealed
) {
    const double target = revealed ? 1.0 : 0.0;
    if (std::abs(target - self->progress) <= kProgressEpsilon && self->tick_id == 0) {
        if (revealed) {
            g_signal_emit(self, signals[kRevealedSignal], 0);
        } else {
            g_signal_emit(self, signals[kConcealedSignal], 0);
        }
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

void notification_reveal_set_revealed_immediately(
    RealmheartNotificationReveal* self,
    bool revealed
) {
    stop_animation(self);
    self->animation_start_progress = revealed ? 1.0 : 0.0;
    self->animation_target_progress = self->animation_start_progress;
    self->animation_start_us = 0;
    self->animation_duration_us = 0;
    set_progress(self, self->animation_target_progress);
}

RealmheartNotificationReveal* as_notification_reveal(GtkWidget* widget) {
    return static_cast<RealmheartNotificationReveal*>(
        reinterpret_cast<void*>(widget)
    );
}

} // namespace

namespace realmheart::ui {

namespace {

constexpr int kToastWidth = 396;
constexpr int kToastHeight = 96;
constexpr guint kOpeningDurationMs = 420;
constexpr guint kClosingDurationMs = 360;

std::string display_app_name(const services::NotificationEntry& entry) {
    if (!entry.app_name.empty()) return entry.app_name;
    return "Notification";
}

std::string display_summary(const services::NotificationEntry& entry) {
    if (!entry.summary.empty()) return entry.summary;
    if (!entry.body.empty()) return "New notification";
    return display_app_name(entry);
}

} // namespace

NotificationToast::NotificationToast(GtkApplication* app) : app_(app) {
    window_ = GTK_WIDGET(gtk_application_window_new(app_));
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window_), kToastWidth, kToastHeight);
    gtk_widget_add_css_class(window_, "realmheart-notification-window");

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(card, kToastWidth, kToastHeight);
    gtk_widget_set_halign(card, GTK_ALIGN_FILL);
    gtk_widget_set_valign(card, GTK_ALIGN_FILL);
    gtk_widget_set_overflow(card, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(card, "realmheart-notification-card");

    icon_ = std::make_unique<bar::widgets::ThemedSvgIcon>(
        "Realmheart-Icons/notifications.svg",
        22
    );
    icon_->add_css_class("realmheart-notification-icon");
    gtk_widget_set_valign(icon_->widget(), GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(icon_->widget(), FALSE);
    gtk_box_append(GTK_BOX(card), icon_->widget());

    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
    gtk_widget_set_overflow(content, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_can_target(content, FALSE);

    label_app_ = gtk_label_new("Notification");
    gtk_label_set_xalign(GTK_LABEL(label_app_), 0.0F);
    gtk_label_set_single_line_mode(GTK_LABEL(label_app_), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label_app_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label_app_), 38);
    gtk_widget_set_halign(label_app_, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(label_app_, "realmheart-notification-app");
    gtk_widget_set_can_target(label_app_, FALSE);

    label_summary_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label_summary_), 0.0F);
    gtk_label_set_single_line_mode(GTK_LABEL(label_summary_), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label_summary_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label_summary_), 38);
    gtk_widget_set_halign(label_summary_, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(label_summary_, "realmheart-notification-summary");
    gtk_widget_set_can_target(label_summary_, FALSE);

    label_body_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label_body_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(label_body_), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label_body_), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(label_body_), 2);
    gtk_label_set_ellipsize(GTK_LABEL(label_body_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(label_body_, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(label_body_, "realmheart-notification-body");
    gtk_widget_set_can_target(label_body_, FALSE);

    gtk_box_append(GTK_BOX(content), label_app_);
    gtk_box_append(GTK_BOX(content), label_summary_);
    gtk_box_append(GTK_BOX(content), label_body_);
    gtk_box_append(GTK_BOX(card), content);

    close_button_ = gtk_button_new_with_label("×");
    gtk_widget_set_halign(close_button_, GTK_ALIGN_END);
    gtk_widget_set_valign(close_button_, GTK_ALIGN_START);
    gtk_widget_add_css_class(close_button_, "realmheart-notification-close");
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(close_button_),
        GTK_ACCESSIBLE_PROPERTY_LABEL,
        "Dismiss notification popup",
        -1
    );
    g_signal_connect(
        close_button_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<NotificationToast*>(data)->dismiss();
        }),
        this
    );
    gtk_box_append(GTK_BOX(card), close_button_);

    reveal_ = notification_reveal_new(
        card,
        close_button_,
        kOpeningDurationMs,
        kClosingDurationMs
    );
    gtk_widget_set_size_request(reveal_, kToastWidth, kToastHeight);
    gtk_window_set_child(GTK_WINDOW(window_), reveal_);

    g_signal_connect(
        reveal_,
        "revealed",
        G_CALLBACK(+[](RealmheartNotificationReveal*, gpointer data) {
            auto* self = static_cast<NotificationToast*>(data);
            if (!self->destroying_ && self->visible_ && !self->closing_) {
                self->schedule_timeout();
            }
        }),
        this
    );
    g_signal_connect(
        reveal_,
        "concealed",
        G_CALLBACK(+[](RealmheartNotificationReveal*, gpointer data) {
            auto* self = static_cast<NotificationToast*>(data);
            if (self->window_ != nullptr) {
                gtk_widget_set_visible(self->window_, FALSE);
            }
            self->visible_ = false;
            self->closing_ = false;
            if (!self->destroying_) self->show_next();
        }),
        this
    );

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-notification";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::None;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.margin_right = 24;
    spec.margin_top = 24;
    apply_layer_surface(GTK_WINDOW(window_), spec);

    notification_reveal_set_revealed_immediately(
        as_notification_reveal(reveal_),
        false
    );
    gtk_widget_set_visible(window_, FALSE);
}

NotificationToast::~NotificationToast() {
    destroying_ = true;
    queue_.clear();
    hide_current();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void NotificationToast::show(
    const services::NotificationEntry& entry,
    int timeout_ms
) {
    constexpr std::size_t max_queue = 20;
    if (queue_.size() >= max_queue) queue_.pop_front();
    queue_.push_back({entry, timeout_ms});
    if (!visible_) show_next();
}

void NotificationToast::show_next() {
    if (destroying_ || visible_ || queue_.empty()) return;

    QueuedToast toast = std::move(queue_.front());
    queue_.pop_front();
    current_timeout_ms_ = toast.timeout_ms;

    const std::string app_name = display_app_name(toast.entry);
    const std::string summary = display_summary(toast.entry);
    gtk_label_set_text(GTK_LABEL(label_app_), app_name.c_str());
    gtk_label_set_text(GTK_LABEL(label_summary_), summary.c_str());
    gtk_label_set_text(GTK_LABEL(label_body_), toast.entry.body.c_str());
    gtk_widget_set_visible(label_body_, !toast.entry.body.empty());

    notification_reveal_set_revealed_immediately(
        as_notification_reveal(reveal_),
        false
    );
    visible_ = true;
    closing_ = false;
    gtk_window_present(GTK_WINDOW(window_));
    notification_reveal_set_revealed(as_notification_reveal(reveal_), true);
}

void NotificationToast::schedule_timeout() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    if (current_timeout_ms_ <= 0) return;

    timeout_id_ = g_timeout_add(
        static_cast<guint>(current_timeout_ms_),
        &NotificationToast::dismiss_timeout,
        this
    );
}

gboolean NotificationToast::dismiss_timeout(gpointer data) {
    auto* self = static_cast<NotificationToast*>(data);
    self->timeout_id_ = 0;
    self->dismiss();
    return G_SOURCE_REMOVE;
}

void NotificationToast::hide_current() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    visible_ = false;
    closing_ = false;
    if (reveal_ != nullptr) {
        notification_reveal_set_revealed_immediately(
            as_notification_reveal(reveal_),
            false
        );
    }
    if (window_ != nullptr) gtk_widget_set_visible(window_, FALSE);
}

void NotificationToast::dismiss() {
    if (timeout_id_ != 0) {
        g_source_remove(timeout_id_);
        timeout_id_ = 0;
    }
    if (destroying_) {
        hide_current();
        return;
    }
    if (!visible_ || closing_ || reveal_ == nullptr) return;

    closing_ = true;
    notification_reveal_set_revealed(as_notification_reveal(reveal_), false);
}

} // namespace realmheart::ui
