#include "ui/components/NotificationWidget.hpp"

#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <algorithm>

namespace realmheart::ui::components {
namespace {

GtkWidget* empty_icon() {
    realmheart::ui::bar::widgets::ThemedSvgIcon icon(
        "Realmheart-Icons/notifications.svg", 31
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

} // namespace

NotificationWidget::NotificationWidget(services::NotificationHistory& history)
    : history_(history) {
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box_, "realmheart-notifications");
    gtk_widget_set_vexpand(box_, FALSE);

    GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_add_css_class(controls, "realmheart-notifications-header");

    GtkWidget* title_group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_hexpand(title_group, TRUE);
    GtkWidget* heading = gtk_label_new("NOTIFICATIONS");
    gtk_widget_add_css_class(heading, "realmheart-notifications-title");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0F);
    gtk_box_append(GTK_BOX(title_group), heading);

    count_label_ = gtk_label_new("0");
    gtk_widget_add_css_class(count_label_, "realmheart-notifications-count");
    gtk_widget_set_valign(count_label_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(title_group), count_label_);
    gtk_box_append(GTK_BOX(controls), title_group);

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
    gtk_box_append(GTK_BOX(controls), clear_button_);
    gtk_box_append(GTK_BOX(box_), controls);

    scroller_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller_, "realmheart-notifications-scroller");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC
    );
    gtk_widget_set_vexpand(scroller_, FALSE);
    gtk_widget_set_size_request(scroller_, -1, 286);
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

    list_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
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
    subscription_.reset();
    state_->alive = false;
    state_->owner = nullptr;
}

GtkWidget* NotificationWidget::get_widget() {
    return box_;
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
    clear_box(list_);

    const auto snapshot = history_.snapshot();
    gtk_widget_set_sensitive(clear_button_, !snapshot.entries.empty());
    gtk_widget_set_visible(count_label_, !snapshot.entries.empty());
    const std::string count = std::to_string(snapshot.entries.size());
    gtk_label_set_text(GTK_LABEL(count_label_), count.c_str());

    if (snapshot.entries.empty()) {
        GtkWidget* empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
        gtk_widget_add_css_class(empty, "realmheart-notifications-empty-state");
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand(empty, TRUE);
        gtk_box_append(GTK_BOX(empty), empty_icon());

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
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "realmheart-notification-row");
        if (entry.unread) gtk_widget_add_css_class(row, "unread");

        GtkWidget* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_add_css_class(copy, "realmheart-notification-copy");
        gtk_widget_set_hexpand(copy, TRUE);
        gtk_widget_set_size_request(copy, 0, -1);

        GtkWidget* meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(meta, "realmheart-notification-meta");

        if (entry.unread) {
            GtkWidget* unread = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(unread, "realmheart-notification-unread-dot");
            gtk_widget_set_size_request(unread, 5, 5);
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

        gtk_box_append(GTK_BOX(list_), row);
    }
}

} // namespace realmheart::ui::components
