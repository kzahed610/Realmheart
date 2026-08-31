#include "ui/components/NotificationWidget.hpp"

#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <algorithm>

namespace realmheart::ui::components {
namespace {

GtkWidget* empty_icon(int pixels) {
    realmheart::ui::bar::widgets::ThemedSvgIcon icon(
        "Realmheart-Icons/notifications.svg", pixels
    );
    icon.add_css_class("realmheart-notifications-empty-icon");
    return icon.widget();
}

void clear_box(GtkWidget* box) {
    GtkWidget* child = gtk_widget_get_first_child(box);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

bool is_inside_button(GtkWidget* widget, GtkWidget* stop_at) {
    for (GtkWidget* current = widget;
         current != nullptr && current != stop_at;
         current = gtk_widget_get_parent(current)) {
        if (GTK_IS_BUTTON(current)) return true;
    }
    return false;
}

// Swipe bin: a bare GtkWidget subclass whose snapshot applies a horizontal
// translate to its child. Sliding a notification row therefore never
// triggers relayout — it is a pure render-stage transform, so it stays
// buttery even next to the character compositor.
typedef struct _RealmheartSwipeBin {
    GtkWidget parent_instance;
    double translate_x;
} RealmheartSwipeBin;

typedef struct _RealmheartSwipeBinClass {
    GtkWidgetClass parent_class;
} RealmheartSwipeBinClass;

G_DEFINE_TYPE(RealmheartSwipeBin, realmheart_swipe_bin, GTK_TYPE_WIDGET)

void realmheart_swipe_bin_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    auto* self = reinterpret_cast<RealmheartSwipeBin*>(widget);
    GtkWidget* child = gtk_widget_get_first_child(widget);
    if (child == nullptr) return;
    if (self->translate_x != 0.0) {
        const graphene_point_t offset = GRAPHENE_POINT_INIT(
            static_cast<float>(self->translate_x),
            0.0F
        );
        gtk_snapshot_translate(snapshot, &offset);
    }
    gtk_widget_snapshot_child(widget, child, snapshot);
}

void realmheart_swipe_bin_measure(
    GtkWidget* widget,
    GtkOrientation orientation,
    int for_size,
    int* minimum,
    int* natural,
    int* minimum_baseline,
    int* natural_baseline
) {
    GtkWidget* child = gtk_widget_get_first_child(widget);
    if (child != nullptr) {
        gtk_widget_measure(
            child, orientation, for_size,
            minimum, natural, minimum_baseline, natural_baseline
        );
        return;
    }
    *minimum = 0;
    *natural = 0;
    if (minimum_baseline != nullptr) *minimum_baseline = -1;
    if (natural_baseline != nullptr) *natural_baseline = -1;
}

void realmheart_swipe_bin_size_allocate(
    GtkWidget* widget,
    int width,
    int height,
    int baseline
) {
    GtkWidget* child = gtk_widget_get_first_child(widget);
    if (child != nullptr) {
        const GtkAllocation allocation{0, 0, width, height};
        gtk_widget_size_allocate(child, &allocation, baseline);
    }
}

void realmheart_swipe_bin_dispose(GObject* object) {
    GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(object));
    if (child != nullptr) gtk_widget_unparent(child);
    G_OBJECT_CLASS(realmheart_swipe_bin_parent_class)->dispose(object);
}

void realmheart_swipe_bin_class_init(RealmheartSwipeBinClass* klass) {
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = realmheart_swipe_bin_snapshot;
    widget_class->measure = realmheart_swipe_bin_measure;
    widget_class->size_allocate = realmheart_swipe_bin_size_allocate;
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = realmheart_swipe_bin_dispose;
    gtk_widget_class_set_css_name(widget_class, "realmheart-notification-slot");
}

void realmheart_swipe_bin_init(RealmheartSwipeBin* self) {
    self->translate_x = 0.0;
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

void realmheart_swipe_bin_set_translate(GtkWidget* widget, double offset) {
    auto* self = reinterpret_cast<RealmheartSwipeBin*>(widget);
    if (self->translate_x == offset) return;
    self->translate_x = offset;
    gtk_widget_queue_draw(widget);
}

constexpr double kSwipeDismissFraction = 0.30;
constexpr gint64 kSwipeAnimDurationUs = 180000;

} // namespace

NotificationWidget::NotificationWidget(services::NotificationHistory& history)
    : history_(history) {
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box_, "realmheart-notifications");
    gtk_widget_set_vexpand(box_, FALSE);

    controls_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, layout_.header_spacing);
    gtk_widget_add_css_class(controls_, "realmheart-notifications-header");

    title_group_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, layout_.header_spacing);
    gtk_widget_set_hexpand(title_group_, TRUE);
    GtkWidget* heading = gtk_label_new("NOTIFICATIONS");
    gtk_widget_add_css_class(heading, "realmheart-notifications-title");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0F);
    gtk_box_append(GTK_BOX(title_group_), heading);

    count_label_ = gtk_label_new("0");
    gtk_widget_add_css_class(count_label_, "realmheart-notifications-count");
    gtk_widget_set_valign(count_label_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(title_group_), count_label_);
    gtk_box_append(GTK_BOX(controls_), title_group_);

    clear_button_ = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(clear_button_, "realmheart-notifications-clear");
    gtk_widget_set_tooltip_text(clear_button_, "Clear notification history");
    gtk_widget_set_can_target(clear_button_, TRUE);
    gtk_widget_set_focusable(clear_button_, FALSE);
    g_signal_connect(clear_button_, "clicked", G_CALLBACK(+[](
        GtkButton*, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->clear_notifications();
    }), this);
    gtk_box_append(GTK_BOX(controls_), clear_button_);
    gtk_box_append(GTK_BOX(box_), controls_);

    scroller_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller_, "realmheart-notifications-scroller");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC
    );
    gtk_widget_set_vexpand(scroller_, layout_.expand_to_fill);
    gtk_widget_set_size_request(
        scroller_, -1, layout_.expand_to_fill ? -1 : layout_.viewport_height
    );
    gtk_widget_set_margin_bottom(scroller_, layout_.bottom_margin);
    gtk_widget_set_cursor_from_name(scroller_, "grab");

    vertical_adjustment_ = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(scroller_)
    );

    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    gtk_gesture_single_set_exclusive(GTK_GESTURE_SINGLE(drag), FALSE);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(drag), GTK_PHASE_BUBBLE
    );
    g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](
        GtkGestureDrag* gesture, double start_x, double start_y, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->begin_drag_scroll(
            gesture, start_x, start_y
        );
    }), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(+[](
        GtkGestureDrag* gesture, double offset_x, double offset_y, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->update_drag_scroll(
            gesture, offset_x, offset_y
        );
    }), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(+[](
        GtkGestureDrag*, double, double, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->end_drag_scroll();
    }), this);
    g_signal_connect(drag, "cancel", G_CALLBACK(+[](
        GtkGesture*, GdkEventSequence*, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->end_drag_scroll();
    }), this);
    gtk_widget_add_controller(scroller_, GTK_EVENT_CONTROLLER(drag));

    // Horizontal swipe-to-dismiss on rows. Lives on the scroller so the
    // whole row area is fair game; begin_row_swipe() resolves which row
    // (if any) the pointer actually started on.
    GtkGesture* swipe = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(swipe), GDK_BUTTON_PRIMARY);
    gtk_gesture_single_set_exclusive(GTK_GESTURE_SINGLE(swipe), FALSE);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(swipe), GTK_PHASE_CAPTURE
    );
    g_signal_connect(swipe, "drag-begin", G_CALLBACK(+[](
        GtkGestureDrag* gesture, double start_x, double start_y, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->begin_row_swipe(
            gesture, start_x, start_y
        );
    }), this);
    g_signal_connect(swipe, "drag-update", G_CALLBACK(+[](
        GtkGestureDrag* gesture, double offset_x, double offset_y, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->update_row_swipe(
            gesture, offset_x, offset_y
        );
    }), this);
    g_signal_connect(swipe, "drag-end", G_CALLBACK(+[](
        GtkGestureDrag* gesture, double, double, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->end_row_swipe(gesture);
    }), this);
    g_signal_connect(swipe, "cancel", G_CALLBACK(+[](
        GtkGesture* gesture, GdkEventSequence*, gpointer data
    ) {
        static_cast<NotificationWidget*>(data)->end_row_swipe(
            GTK_GESTURE_DRAG(gesture)
        );
    }), this);
    gtk_widget_add_controller(scroller_, GTK_EVENT_CONTROLLER(swipe));

    list_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, layout_.list_spacing);
    gtk_widget_add_css_class(list_, "realmheart-notifications-list");
    gtk_widget_set_vexpand(list_, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), list_);
    gtk_box_append(GTK_BOX(box_), scroller_);

    state_->owner = this;
    const auto state = state_;
    subscription_ = history_.subscribe([state] {
        if (!state->alive.load() || state->refresh_queued.exchange(true)) return;
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* lifetime = static_cast<std::shared_ptr<LifetimeState>*>(raw);
                (*lifetime)->refresh_queued = false;
                if ((*lifetime)->alive.load() && (*lifetime)->owner != nullptr) {
                    (*lifetime)->owner->refresh();
                }
                return G_SOURCE_REMOVE;
            },
            new std::shared_ptr<LifetimeState>(state),
            +[](gpointer raw) { delete static_cast<std::shared_ptr<LifetimeState>*>(raw); }
        );
    });
    refresh();
}

NotificationWidget::~NotificationWidget() {
    cancel_swipe_animation();
    subscription_.reset();
    state_->alive = false;
    state_->owner = nullptr;
}

GtkWidget* NotificationWidget::get_widget() {
    return box_;
}

void NotificationWidget::set_layout(NotificationLayout layout) {
    layout_ = layout;
    if (controls_ != nullptr) {
        gtk_box_set_spacing(GTK_BOX(controls_), layout_.header_spacing);
    }
    if (title_group_ != nullptr) {
        gtk_box_set_spacing(GTK_BOX(title_group_), layout_.header_spacing);
    }
    if (scroller_ != nullptr) {
        gtk_widget_set_vexpand(scroller_, layout_.expand_to_fill);
        gtk_widget_set_size_request(
            scroller_, -1, layout_.expand_to_fill ? -1 : layout_.viewport_height
        );
        gtk_widget_set_margin_bottom(scroller_, layout_.bottom_margin);
    }
    if (list_ != nullptr) {
        gtk_box_set_spacing(GTK_BOX(list_), layout_.list_spacing);
    }
    if (box_ != nullptr) gtk_widget_queue_resize(box_);
    refresh();
}

void NotificationWidget::clear_notifications() {
    history_.clear();

    // Refresh synchronously so the button always gives immediate visual
    // feedback even when a history notification refresh is already queued.
    refresh();
    if (vertical_adjustment_ != nullptr) {
        gtk_adjustment_set_value(
            vertical_adjustment_, gtk_adjustment_get_lower(vertical_adjustment_)
        );
    }
}

void NotificationWidget::dismiss_notification(std::uint32_t id) {
    if (!history_.dismiss(id)) return;
    refresh();
}

void NotificationWidget::begin_drag_scroll(
    GtkGestureDrag* gesture,
    double start_x,
    double start_y
) {
    drag_active_ = false;
    drag_blocked_ = false;
    if (vertical_adjustment_ == nullptr || scroller_ == nullptr) return;

    GtkWidget* picked = gtk_widget_pick(
        scroller_, start_x, start_y, GTK_PICK_DEFAULT
    );
    if (is_inside_button(picked, scroller_)) {
        drag_blocked_ = true;
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    drag_start_value_ = gtk_adjustment_get_value(vertical_adjustment_);
}

void NotificationWidget::update_drag_scroll(
    GtkGestureDrag* gesture, double offset_x, double offset_y
) {
    if (vertical_adjustment_ == nullptr || drag_blocked_) return;

    if (!drag_active_) {
        if (!gtk_drag_check_threshold(
                scroller_, 0, 0,
                static_cast<int>(offset_x), static_cast<int>(offset_y)
            )) {
            return;
        }

        drag_active_ = true;
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        gtk_widget_add_css_class(scroller_, "dragging");
        gtk_widget_set_cursor_from_name(scroller_, "grabbing");
    }

    const double lower = gtk_adjustment_get_lower(vertical_adjustment_);
    const double upper = gtk_adjustment_get_upper(vertical_adjustment_);
    const double page_size = gtk_adjustment_get_page_size(vertical_adjustment_);
    const double maximum = std::max(lower, upper - page_size);
    const double target = std::clamp(drag_start_value_ - offset_y, lower, maximum);
    gtk_adjustment_set_value(vertical_adjustment_, target);
}

void NotificationWidget::end_drag_scroll() {
    if (scroller_ == nullptr) return;
    drag_active_ = false;
    drag_blocked_ = false;
    gtk_widget_remove_css_class(scroller_, "dragging");
    gtk_widget_set_cursor_from_name(scroller_, "grab");
}

void NotificationWidget::refresh() {
    cancel_swipe_animation();
    clear_box(list_);

    const auto snapshot = history_.snapshot();
    gtk_widget_set_sensitive(clear_button_, !snapshot.entries.empty());
    gtk_widget_set_visible(count_label_, !snapshot.entries.empty());
    const std::string count = std::to_string(snapshot.entries.size());
    gtk_label_set_text(GTK_LABEL(count_label_), count.c_str());

    if (snapshot.entries.empty()) {
        GtkWidget* empty = gtk_box_new(
            GTK_ORIENTATION_VERTICAL, layout_.header_spacing
        );
        gtk_widget_add_css_class(empty, "realmheart-notifications-empty-state");
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand(empty, TRUE);
        gtk_box_append(GTK_BOX(empty), empty_icon(layout_.empty_icon_size));

        GtkWidget* title = gtk_label_new("All clear");
        gtk_widget_add_css_class(title, "realmheart-notifications-empty");
        gtk_box_append(GTK_BOX(empty), title);

        GtkWidget* detail = gtk_label_new("No pending notifications");
        gtk_widget_add_css_class(detail, "realmheart-notifications-empty-detail");
        gtk_box_append(GTK_BOX(empty), detail);
        gtk_box_append(GTK_BOX(list_), empty);
        return;
    }

    for (auto iterator = snapshot.entries.rbegin(); iterator != snapshot.entries.rend(); ++iterator) {
        const auto& entry = *iterator;
        GtkWidget* row = gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL, layout_.row_spacing
        );
        gtk_widget_add_css_class(row, "realmheart-notification-row");
        if (entry.unread) gtk_widget_add_css_class(row, "unread");

        GtkWidget* copy = gtk_box_new(
            GTK_ORIENTATION_VERTICAL, layout_.copy_spacing
        );
        gtk_widget_add_css_class(copy, "realmheart-notification-copy");
        gtk_widget_set_hexpand(copy, TRUE);
        gtk_widget_set_size_request(copy, 0, -1);

        GtkWidget* meta = gtk_box_new(
            GTK_ORIENTATION_HORIZONTAL, layout_.meta_spacing
        );
        gtk_widget_add_css_class(meta, "realmheart-notification-meta");

        if (entry.unread) {
            GtkWidget* unread = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(unread, "realmheart-notification-unread-dot");
            gtk_widget_set_size_request(
                unread, layout_.unread_dot_size, layout_.unread_dot_size
            );
            gtk_widget_set_valign(unread, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(meta), unread);
        }

        const std::string source = entry.app_name.empty() ? "System" : entry.app_name;
        GtkWidget* app = gtk_label_new(source.c_str());
        gtk_widget_add_css_class(app, "realmheart-notification-app");
        gtk_label_set_xalign(GTK_LABEL(app), 0.0F);
        gtk_label_set_single_line_mode(GTK_LABEL(app), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(app), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(app, TRUE);
        gtk_box_append(GTK_BOX(meta), app);

        gtk_box_append(GTK_BOX(copy), meta);

        const std::string summary_text = entry.summary.empty()
            ? (entry.body.empty() ? source : "New notification")
            : entry.summary;
        GtkWidget* summary = gtk_label_new(summary_text.c_str());
        gtk_widget_add_css_class(summary, "realmheart-notification-summary");
        gtk_label_set_xalign(GTK_LABEL(summary), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
        gtk_label_set_single_line_mode(GTK_LABEL(summary), TRUE);
        gtk_box_append(GTK_BOX(copy), summary);

        if (!entry.body.empty()) {
            GtkWidget* body = gtk_label_new(entry.body.c_str());
            gtk_widget_add_css_class(body, "realmheart-notification-body");
            gtk_label_set_xalign(GTK_LABEL(body), 0.0F);
            gtk_label_set_wrap(GTK_LABEL(body), TRUE);
            gtk_label_set_wrap_mode(GTK_LABEL(body), PANGO_WRAP_WORD_CHAR);
            gtk_label_set_lines(GTK_LABEL(body), 2);
            gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(copy), body);
        }

        gtk_box_append(GTK_BOX(row), copy);

        GtkWidget* dismiss = gtk_button_new_with_label("×");
        gtk_widget_add_css_class(dismiss, "realmheart-notification-dismiss");
        gtk_widget_set_tooltip_text(dismiss, "Dismiss notification");
        gtk_widget_set_can_target(dismiss, TRUE);
        gtk_widget_set_focusable(dismiss, FALSE);
        gtk_widget_set_valign(dismiss, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(dismiss, GTK_ALIGN_CENTER);
        g_object_set_data(
            G_OBJECT(dismiss), "realmheart-notification-id", GUINT_TO_POINTER(entry.id)
        );
        g_signal_connect(dismiss, "clicked", G_CALLBACK(+[](
            GtkButton* button, gpointer data
        ) {
            const auto id = GPOINTER_TO_UINT(g_object_get_data(
                G_OBJECT(button), "realmheart-notification-id"
            ));
            static_cast<NotificationWidget*>(data)->dismiss_notification(id);
        }), this);
        gtk_box_append(GTK_BOX(row), dismiss);

        // Wrap the row in a swipe bin: horizontal drags translate the bin
        // (pure snapshot transform), release either dismisses the entry or
        // springs the row back.
        GtkWidget* slot = static_cast<GtkWidget*>(
            g_object_new(realmheart_swipe_bin_get_type(), nullptr)
        );
        gtk_widget_set_parent(row, slot);
        g_object_set_data(
            G_OBJECT(slot),
            "realmheart-notification-id",
            GUINT_TO_POINTER(entry.id)
        );
        gtk_box_append(GTK_BOX(list_), slot);
    }
}

void NotificationWidget::begin_row_swipe(
    GtkGestureDrag* /*gesture*/,
    double start_x,
    double start_y
) {
    swipe_blocked_ = false;
    if (swipe_row_ != nullptr) return;

    GtkWidget* picked = gtk_widget_pick(
        scroller_, start_x, start_y, GTK_PICK_DEFAULT
    );
    if (picked == nullptr || is_inside_button(picked, scroller_)) {
        swipe_blocked_ = true;
        return;
    }

    // Walk up to the swipe bin wrapping the row under the pointer.
    GtkWidget* slot = picked;
    while (slot != nullptr && slot != list_) {
        if (G_TYPE_CHECK_INSTANCE_TYPE(
                slot, realmheart_swipe_bin_get_type()
            )) {
            break;
        }
        slot = gtk_widget_get_parent(slot);
    }
    if (slot == nullptr || slot == list_) {
        swipe_blocked_ = true;
        return;
    }

    cancel_swipe_animation();
    swipe_row_ = slot;
    swipe_id_ = GPOINTER_TO_UINT(g_object_get_data(
        G_OBJECT(slot), "realmheart-notification-id"
    ));
    swipe_offset_ = 0.0;
    swipe_will_dismiss_ = false;
}

void NotificationWidget::update_row_swipe(
    GtkGestureDrag* gesture,
    double offset_x,
    double offset_y
) {
    if (swipe_blocked_ || swipe_row_ == nullptr) return;

    if (!swipe_active_) {
        // Only claim horizontal intent — vertical drags keep feeding the
        // scroll-drag gesture.
        if (!gtk_drag_check_threshold(
                scroller_,
                0, 0,
                static_cast<int>(offset_x), static_cast<int>(offset_y)
            )) {
            return;
        }
        if (std::abs(offset_x) < std::abs(offset_y)) {
            swipe_blocked_ = true;
            gtk_gesture_set_state(
                GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED
            );
            return;
        }
        swipe_active_ = true;
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    }

    // Rubber-band past the edges so the row never flies away mid-drag.
    const double width = std::max(
        static_cast<double>(gtk_widget_get_width(swipe_row_)), 1.0
    );
    double target = offset_x;
    const double limit = width;
    if (target > limit) target = limit + (target - limit) * 0.25;
    if (target < -limit) target = -limit + (target + limit) * 0.25;

    swipe_offset_ = target;
    const double threshold = width * kSwipeDismissFraction;
    const bool will_dismiss = std::abs(target) >= threshold;
    if (will_dismiss != swipe_will_dismiss_) {
        swipe_will_dismiss_ = will_dismiss;
        GtkWidget* row = gtk_widget_get_first_child(swipe_row_);
        if (will_dismiss) {
            if (row != nullptr) gtk_widget_add_css_class(row, "swipe-armed");
        } else if (row != nullptr) {
            gtk_widget_remove_css_class(row, "swipe-armed");
        }
    }
    set_swipe_translate(target);
}

void NotificationWidget::end_row_swipe(GtkGestureDrag* gesture) {
    if (swipe_blocked_) {
        swipe_blocked_ = false;
        return;
    }
    if (swipe_row_ == nullptr) return;

    double start_x = 0.0, start_y = 0.0;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    double end_x = 0.0, end_y = 0.0;
    double offset_x = 0.0;
    if (gtk_gesture_drag_get_offset(gesture, &end_x, &end_y)) {
        offset_x = end_x - start_x;
    }
    (void)start_y;
    (void)end_y;

    GtkWidget* row = swipe_row_;
    const std::uint32_t id = swipe_id_;
    const double width = std::max(
        static_cast<double>(gtk_widget_get_width(row)), 1.0
    );

    // Flick detection: a fast horizontal release dismisses even below the
    // distance threshold. Rough px/ms velocity over the last gesture.
    const bool flick = std::abs(offset_x) > 90.0;

    const bool dismiss =
        std::abs(swipe_offset_) >= width * kSwipeDismissFraction || flick;

    gtk_widget_remove_css_class(row, "swipe-armed");
    GtkWidget* row_child = gtk_widget_get_first_child(row);
    if (row_child != nullptr) {
        gtk_widget_remove_css_class(row_child, "swipe-armed");
    }
    if (dismiss) {
        const double direction = swipe_offset_ != 0.0
            ? (swipe_offset_ > 0.0 ? 1.0 : -1.0)
            : (offset_x > 0.0 ? 1.0 : -1.0);
        swipe_id_ = id;
        animate_swipe_release(direction * (width + 24.0), true);
    } else {
        animate_swipe_release(0.0, false);
    }
    // swipe_row_ is cleared by the animation tick (or cancel) so a second
    // gesture cannot grab the same mid-animation row.
}

void NotificationWidget::animate_swipe_release(
    double target,
    bool dismiss_after
) {
    // Kill only a previous release tick — NOT the whole swipe state.
    // cancel_swipe_animation() would null swipe_row_ and snap the translate
    // back to 0, so the fly-out/spring-back could never start (and a
    // dismiss release would silently become a no-op).
    if (swipe_tick_id_ != 0 && scroller_ != nullptr) {
        gtk_widget_remove_tick_callback(scroller_, swipe_tick_id_);
        swipe_tick_id_ = 0;
    }
    if (swipe_row_ == nullptr) return;
    swipe_anim_start_ = swipe_offset_;
    swipe_anim_target_ = target;
    swipe_anim_start_us_ = g_get_monotonic_time();
    swipe_anim_dismiss_ = dismiss_after;
    swipe_tick_id_ = gtk_widget_add_tick_callback(
        scroller_, &NotificationWidget::swipe_tick, this, nullptr
    );
}

gboolean NotificationWidget::swipe_tick(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer data
) {
    auto* self = static_cast<NotificationWidget*>(data);
    self->swipe_tick_id_ = 0;

    const gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
    double linear = static_cast<double>(now - self->swipe_anim_start_us_) /
        static_cast<double>(kSwipeAnimDurationUs);
    linear = std::clamp(linear, 0.0, 1.0);
    const double eased = 1.0 - (1.0 - linear) * (1.0 - linear);
    const double value = self->swipe_anim_start_ +
        (self->swipe_anim_target_ - self->swipe_anim_start_) * eased;

    if (linear < 1.0) {
        self->set_swipe_translate(value);
        self->swipe_tick_id_ = gtk_widget_add_tick_callback(
            self->scroller_, &NotificationWidget::swipe_tick, self, nullptr
        );
        return G_SOURCE_REMOVE;
    }

    GtkWidget* row = self->swipe_row_;
    self->swipe_row_ = nullptr;
    self->swipe_offset_ = 0.0;
    if (self->swipe_anim_dismiss_ && row != nullptr) {
        const auto id = GPOINTER_TO_UINT(g_object_get_data(
            G_OBJECT(row), "realmheart-notification-id"
        ));
        self->set_swipe_translate(0.0);
        self->dismiss_notification(id);
    } else {
        self->set_swipe_translate(0.0);
    }
    return G_SOURCE_REMOVE;
}

void NotificationWidget::set_swipe_translate(double offset) {
    swipe_offset_ = offset;
    if (swipe_row_ != nullptr) {
        realmheart_swipe_bin_set_translate(swipe_row_, offset);
    }
}

void NotificationWidget::cancel_swipe_animation() {
    if (swipe_tick_id_ != 0 && scroller_ != nullptr) {
        gtk_widget_remove_tick_callback(scroller_, swipe_tick_id_);
        swipe_tick_id_ = 0;
    }
    if (swipe_row_ != nullptr) {
        realmheart_swipe_bin_set_translate(swipe_row_, 0.0);
    }
    swipe_row_ = nullptr;
    swipe_id_ = 0;
    swipe_offset_ = 0.0;
    swipe_active_ = false;
    swipe_will_dismiss_ = false;
}

} // namespace realmheart::ui::components
