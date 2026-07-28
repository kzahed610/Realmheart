#include "effects/shell/ShellEffectView.hpp"

#include <algorithm>
#include <cmath>

struct _RealmheartShellEffectView {
    GtkWidget parent_instance;

    GtkWidget* child = nullptr;
    realmheart::effects::EffectFrame frame{};
    double origin_x = 0.5;
    double origin_y = 0.5;
};

G_DEFINE_TYPE(
    RealmheartShellEffectView,
    realmheart_shell_effect_view,
    GTK_TYPE_WIDGET
)

namespace {

constexpr double kFrameEpsilon = 0.0001;

inline bool nearly_equal(double lhs, double rhs) noexcept {
    return std::abs(lhs - rhs) <= kFrameEpsilon;
}

inline bool is_identity_frame(
    const realmheart::effects::EffectFrame& frame
) noexcept {
    return nearly_equal(frame.opacity, 1.0) &&
        nearly_equal(frame.scale_x, 1.0) &&
        nearly_equal(frame.scale_y, 1.0) &&
        nearly_equal(frame.translate_x, 0.0) &&
        nearly_equal(frame.translate_y, 0.0);
}

inline bool frames_equal(
    const realmheart::effects::EffectFrame& lhs,
    const realmheart::effects::EffectFrame& rhs
) noexcept {
    return nearly_equal(lhs.opacity, rhs.opacity) &&
        nearly_equal(lhs.scale_x, rhs.scale_x) &&
        nearly_equal(lhs.scale_y, rhs.scale_y) &&
        nearly_equal(lhs.translate_x, rhs.translate_x) &&
        nearly_equal(lhs.translate_y, rhs.translate_y);
}

inline double clamp_unit(double value, double fallback) noexcept {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, 0.0, 1.0);
}

inline double sanitize_scale(double value) noexcept {
    if (!std::isfinite(value)) return 1.0;
    return std::max(value, 0.0);
}

inline double sanitize_translation(double value) noexcept {
    return std::isfinite(value) ? value : 0.0;
}

void effect_view_measure(
    GtkWidget* widget,
    GtkOrientation orientation,
    int for_size,
    int* minimum,
    int* natural,
    int* minimum_baseline,
    int* natural_baseline
) {
    auto* self = REALMHEART_SHELL_EFFECT_VIEW(widget);
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

void effect_view_size_allocate(
    GtkWidget* widget,
    int width,
    int height,
    int baseline
) {
    auto* self = REALMHEART_SHELL_EFFECT_VIEW(widget);
    if (self->child == nullptr) return;
    gtk_widget_allocate(self->child, width, height, baseline, nullptr);
}

void effect_view_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    auto* self = REALMHEART_SHELL_EFFECT_VIEW(widget);
    if (self->child == nullptr || self->frame.opacity <= kFrameEpsilon ||
        self->frame.scale_x <= kFrameEpsilon ||
        self->frame.scale_y <= kFrameEpsilon) {
        return;
    }

    // At rest, snapshot the child directly. Avoiding transform/opacity render
    // nodes here is important: those nodes can make GTK/GSK allocate and retain
    // an offscreen texture even after a short transition has completed.
    if (is_identity_frame(self->frame)) {
        gtk_widget_snapshot_child(widget, self->child, snapshot);
        return;
    }

    const float width = static_cast<float>(std::max(gtk_widget_get_width(widget), 0));
    const float height = static_cast<float>(std::max(gtk_widget_get_height(widget), 0));
    const float origin_x = width * static_cast<float>(self->origin_x);
    const float origin_y = height * static_cast<float>(self->origin_y);

    const bool has_transform =
        !nearly_equal(self->frame.scale_x, 1.0) ||
        !nearly_equal(self->frame.scale_y, 1.0) ||
        !nearly_equal(self->frame.translate_x, 0.0) ||
        !nearly_equal(self->frame.translate_y, 0.0);
    const bool has_opacity = !nearly_equal(self->frame.opacity, 1.0);

    if (has_transform) {
        gtk_snapshot_save(snapshot);

        graphene_point_t translation = GRAPHENE_POINT_INIT(
            origin_x + static_cast<float>(self->frame.translate_x),
            origin_y + static_cast<float>(self->frame.translate_y)
        );
        gtk_snapshot_translate(snapshot, &translation);
        gtk_snapshot_scale(
            snapshot,
            static_cast<float>(self->frame.scale_x),
            static_cast<float>(self->frame.scale_y)
        );
        translation = GRAPHENE_POINT_INIT(-origin_x, -origin_y);
        gtk_snapshot_translate(snapshot, &translation);
    }

    if (has_opacity) {
        gtk_snapshot_push_opacity(
            snapshot,
            static_cast<float>(std::clamp(self->frame.opacity, 0.0, 1.0))
        );
    }

    gtk_widget_snapshot_child(widget, self->child, snapshot);

    if (has_opacity) gtk_snapshot_pop(snapshot);
    if (has_transform) gtk_snapshot_restore(snapshot);
}

gboolean effect_view_contains(GtkWidget* widget, double x, double y) {
    auto* self = REALMHEART_SHELL_EFFECT_VIEW(widget);
    if (self->child == nullptr || self->frame.opacity <= kFrameEpsilon ||
        self->frame.scale_x <= kFrameEpsilon ||
        self->frame.scale_y <= kFrameEpsilon) {
        return FALSE;
    }

    const double width = static_cast<double>(std::max(gtk_widget_get_width(widget), 0));
    const double height = static_cast<double>(std::max(gtk_widget_get_height(widget), 0));
    const double origin_x = width * self->origin_x;
    const double origin_y = height * self->origin_y;

    const double child_x = origin_x +
        ((x - self->frame.translate_x - origin_x) / self->frame.scale_x);
    const double child_y = origin_y +
        ((y - self->frame.translate_y - origin_y) / self->frame.scale_y);
    return gtk_widget_contains(self->child, child_x, child_y);
}

void effect_view_dispose(GObject* object) {
    auto* self = REALMHEART_SHELL_EFFECT_VIEW(object);
    if (self->child != nullptr) {
        gtk_widget_unparent(self->child);
        self->child = nullptr;
    }
    G_OBJECT_CLASS(realmheart_shell_effect_view_parent_class)->dispose(object);
}

} // namespace

static void realmheart_shell_effect_view_class_init(
    RealmheartShellEffectViewClass* klass
) {
    auto* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = effect_view_dispose;

    auto* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = effect_view_measure;
    widget_class->size_allocate = effect_view_size_allocate;
    widget_class->snapshot = effect_view_snapshot;
    widget_class->contains = effect_view_contains;
}

static void realmheart_shell_effect_view_init(RealmheartShellEffectView*) {}

GtkWidget* realmheart_shell_effect_view_new(GtkWidget* child) {
    g_return_val_if_fail(GTK_IS_WIDGET(child), nullptr);

    auto* self = REALMHEART_SHELL_EFFECT_VIEW(
        g_object_new(REALMHEART_TYPE_SHELL_EFFECT_VIEW, nullptr)
    );
    self->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

namespace realmheart::effects::shell {

void set_frame(
    RealmheartShellEffectView* view,
    const EffectFrame& frame
) noexcept {
    g_return_if_fail(REALMHEART_IS_SHELL_EFFECT_VIEW(view));

    const EffectFrame next_frame{
        .opacity = clamp_unit(frame.opacity, 1.0),
        .scale_x = sanitize_scale(frame.scale_x),
        .scale_y = sanitize_scale(frame.scale_y),
        .translate_x = sanitize_translation(frame.translate_x),
        .translate_y = sanitize_translation(frame.translate_y),
    };

    if (frames_equal(view->frame, next_frame)) return;

    view->frame = next_frame;
    gtk_widget_queue_draw(GTK_WIDGET(view));
}

void set_origin(
    RealmheartShellEffectView* view,
    double normalized_x,
    double normalized_y
) noexcept {
    g_return_if_fail(REALMHEART_IS_SHELL_EFFECT_VIEW(view));

    const double next_x = clamp_unit(normalized_x, 0.5);
    const double next_y = clamp_unit(normalized_y, 0.5);
    if (nearly_equal(view->origin_x, next_x) &&
        nearly_equal(view->origin_y, next_y)) {
        return;
    }

    view->origin_x = next_x;
    view->origin_y = next_y;
    gtk_widget_queue_draw(GTK_WIDGET(view));
}

} // namespace realmheart::effects::shell
