#include "ui/bar/widgets/WorkspaceRune.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace realmheart::ui::bar::widgets {
namespace {

GdkRGBA foreground(GtkWidget* widget, const GdkRGBA& fallback) {
    GdkRGBA color = fallback;
    gtk_widget_get_color(widget, &color);
    return color;
}

void rune_path(cairo_t* cr, double width, double height) {
    const double cx = width / 2.0;
    cairo_move_to(cr, cx, 1.0);
    cairo_curve_to(cr, cx + 2.2, 4.0, width - 5.0, 4.4, width - 2.0, 7.8);
    cairo_curve_to(cr, width - 5.4, 11.2, width - 5.2, 14.0, width - 3.0, height / 2.0);
    cairo_curve_to(cr, width - 5.2, height - 14.0, width - 5.4, height - 11.2, width - 2.0, height - 7.8);
    cairo_curve_to(cr, width - 5.0, height - 4.4, cx + 2.2, height - 4.0, cx, height - 1.0);
    cairo_curve_to(cr, cx - 2.2, height - 4.0, 5.0, height - 4.4, 2.0, height - 7.8);
    cairo_curve_to(cr, 5.4, height - 11.2, 5.2, height - 14.0, 3.0, height / 2.0);
    cairo_curve_to(cr, 5.2, 14.0, 5.4, 11.2, 2.0, 7.8);
    cairo_curve_to(cr, 5.0, 4.4, cx - 2.2, 4.0, cx, 1.0);
    cairo_close_path(cr);
}

void clear_box(GtkWidget* box) {
    GtkWidget* child = gtk_widget_get_first_child(box);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

} // namespace

WorkspaceRune::WorkspaceRune(
    services::WorkspaceState state,
    std::function<void(int)> on_activate,
    std::function<void()> on_right_click,
    std::function<void(GtkPopover*)> request_exclusive_open
) : state_(std::move(state)),
    on_activate_(std::move(on_activate)),
    on_right_click_(std::move(on_right_click)),
    request_exclusive_open_(std::move(request_exclusive_open)) {
    button_ = gtk_button_new();
    gtk_widget_add_css_class(button_, "realmheart-workspace-rune");
    gtk_widget_set_halign(button_, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(button_, 34, 38);
    gtk_widget_set_focusable(button_, FALSE);

    drawing_area_ = gtk_drawing_area_new();
    gtk_widget_add_css_class(drawing_area_, "realmheart-workspace-rune-art");
    gtk_widget_set_size_request(drawing_area_, 25, 31);
    gtk_widget_set_can_target(drawing_area_, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area_), &WorkspaceRune::draw, this, nullptr);
    gtk_button_set_child(GTK_BUTTON(button_), drawing_area_);

    g_signal_connect(button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<WorkspaceRune*>(data);
        if (self->on_activate_) self->on_activate_(self->state_.id);
    }), this);

    GtkGesture* right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(right_click), GTK_PHASE_CAPTURE
    );
    g_signal_connect(right_click, "pressed", G_CALLBACK(+[](
        GtkGestureClick*, int, double, double, gpointer data
    ) {
        auto* self = static_cast<WorkspaceRune*>(data);
        if (self->on_right_click_) self->on_right_click_();
    }), this);
    gtk_widget_add_controller(button_, GTK_EVENT_CONTROLLER(right_click));

    popover_ = gtk_popover_new();
    gtk_widget_add_css_class(popover_, "realmheart-workspace-preview");
    gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_RIGHT);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(popover_), FALSE);
    gtk_popover_set_offset(GTK_POPOVER(popover_), 8, -5);
    gtk_widget_set_parent(popover_, button_);

    preview_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_popover_set_child(GTK_POPOVER(popover_), preview_box_);

    GtkEventController* button_motion = gtk_event_controller_motion_new();
    g_signal_connect(button_motion, "enter", G_CALLBACK(+[](
        GtkEventControllerMotion*, double, double, gpointer data
    ) {
        static_cast<WorkspaceRune*>(data)->show_preview();
    }), this);
    g_signal_connect(button_motion, "leave", G_CALLBACK(+[](
        GtkEventControllerMotion*, gpointer data
    ) {
        static_cast<WorkspaceRune*>(data)->schedule_preview_hide();
    }), this);
    gtk_widget_add_controller(button_, button_motion);

    GtkEventController* popover_motion = gtk_event_controller_motion_new();
    g_signal_connect(popover_motion, "enter", G_CALLBACK(+[](
        GtkEventControllerMotion*, double, double, gpointer data
    ) {
        static_cast<WorkspaceRune*>(data)->cancel_preview_hide();
    }), this);
    g_signal_connect(popover_motion, "leave", G_CALLBACK(+[](
        GtkEventControllerMotion*, gpointer data
    ) {
        static_cast<WorkspaceRune*>(data)->schedule_preview_hide();
    }), this);
    gtk_widget_add_controller(popover_, popover_motion);

    update(state_);
}

WorkspaceRune::~WorkspaceRune() {
    cancel_preview_hide();
    if (popover_ != nullptr && gtk_widget_get_parent(popover_) != nullptr) {
        gtk_widget_unparent(popover_);
    }
}

void WorkspaceRune::update(const services::WorkspaceState& state) {
    state_ = state;
    gtk_widget_remove_css_class(drawing_area_, "realmheart-workspace-rune-active");
    gtk_widget_remove_css_class(drawing_area_, "realmheart-workspace-rune-occupied");
    gtk_widget_remove_css_class(drawing_area_, "realmheart-workspace-rune-empty");
    gtk_widget_add_css_class(
        drawing_area_,
        state_.active ? "realmheart-workspace-rune-active"
                      : (state_.windows > 0
                          ? "realmheart-workspace-rune-occupied"
                          : "realmheart-workspace-rune-empty")
    );
    gtk_widget_queue_draw(drawing_area_);

    // The styled hover preview is the sole workspace information surface.
    // Keeping a GTK tooltip here creates a second default white tooltip after
    // the pointer rests on the rune, so explicitly leave tooltips disabled.
    gtk_widget_set_tooltip_text(button_, nullptr);
    rebuild_preview();
}

void WorkspaceRune::draw(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
    const auto* self = static_cast<WorkspaceRune*>(data);
    const bool active = self->state_.active;
    const bool occupied = self->state_.windows > 0;

    GdkRGBA accent = foreground(
        GTK_WIDGET(area), GdkRGBA{0.72, 0.42, 0.95, 1.0}
    );

    if (active) {
        GdkRGBA glow = accent;
        glow.alpha = 0.22;
        rune_path(cr, width, height);
        gdk_cairo_set_source_rgba(cr, &glow);
        cairo_fill(cr);
    }

    rune_path(cr, width, height);
    GdkRGBA outline = accent;
    outline.alpha = active ? 0.98 : (occupied ? 0.74 : 0.42);
    gdk_cairo_set_source_rgba(cr, &outline);
    cairo_set_line_width(cr, active ? 1.6 : 1.15);
    cairo_stroke(cr);

    const double cx = width / 2.0;
    const double cy = height / 2.0;
    if (occupied) {
        GdkRGBA mark = accent;
        mark.alpha = active ? 1.0 : 0.78;
        gdk_cairo_set_source_rgba(cr, &mark);
        cairo_set_line_width(cr, active ? 1.5 : 1.15);
        cairo_move_to(cr, cx, cy - 5.0);
        cairo_line_to(cr, cx + 2.2, cy - 1.5);
        cairo_line_to(cr, cx + 5.0, cy);
        cairo_line_to(cr, cx + 2.2, cy + 1.5);
        cairo_line_to(cr, cx, cy + 5.0);
        cairo_line_to(cr, cx - 2.2, cy + 1.5);
        cairo_line_to(cr, cx - 5.0, cy);
        cairo_line_to(cr, cx - 2.2, cy - 1.5);
        cairo_close_path(cr);
        if (active) cairo_fill(cr); else cairo_stroke(cr);
    } else {
        GdkRGBA mark = accent;
        mark.alpha = active ? 0.92 : 0.45;
        gdk_cairo_set_source_rgba(cr, &mark);
        cairo_set_line_width(cr, 1.1);
        cairo_arc(cr, cx, cy, active ? 2.2 : 1.7, 0.0, 2.0 * std::acos(-1.0));
        cairo_stroke(cr);
    }

    if (active && occupied) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_arc(cr, cx, cy, 1.25, 0.0, 2.0 * std::acos(-1.0));
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

void WorkspaceRune::rebuild_preview() {
    clear_box(preview_box_);

    GtkWidget* header = gtk_label_new(("Workspace " + std::to_string(state_.id)).c_str());
    gtk_widget_add_css_class(header, "realmheart-workspace-preview-title");
    gtk_label_set_xalign(GTK_LABEL(header), 0.0F);
    gtk_box_append(GTK_BOX(preview_box_), header);

    if (state_.window_details.empty()) {
        GtkWidget* empty = gtk_label_new("No open windows");
        gtk_widget_add_css_class(empty, "realmheart-workspace-preview-empty");
        gtk_label_set_xalign(GTK_LABEL(empty), 0.0F);
        gtk_box_append(GTK_BOX(preview_box_), empty);
        return;
    }

    const std::size_t count = std::min<std::size_t>(5, state_.window_details.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto& window = state_.window_details[index];
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(row, "realmheart-workspace-preview-row");

        GtkWidget* app = gtk_label_new(window.app_id.c_str());
        gtk_widget_add_css_class(app, "realmheart-workspace-preview-app");
        gtk_label_set_xalign(GTK_LABEL(app), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(app), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(app), 28);

        GtkWidget* title = gtk_label_new(window.title.c_str());
        gtk_widget_add_css_class(title, "realmheart-workspace-preview-window-title");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(title), 34);

        gtk_box_append(GTK_BOX(row), app);
        gtk_box_append(GTK_BOX(row), title);
        gtk_box_append(GTK_BOX(preview_box_), row);
    }
}

void WorkspaceRune::show_preview() {
    cancel_preview_hide();
    if (request_exclusive_open_) request_exclusive_open_(GTK_POPOVER(popover_));
    gtk_popover_popup(GTK_POPOVER(popover_));
}

void WorkspaceRune::schedule_preview_hide() {
    cancel_preview_hide();
    hide_timer_id_ = g_timeout_add(140, +[](gpointer data) -> gboolean {
        auto* self = static_cast<WorkspaceRune*>(data);
        self->hide_timer_id_ = 0;
        gtk_popover_popdown(GTK_POPOVER(self->popover_));
        return G_SOURCE_REMOVE;
    }, this);
}

void WorkspaceRune::cancel_preview_hide() {
    if (hide_timer_id_ != 0) {
        g_source_remove(hide_timer_id_);
        hide_timer_id_ = 0;
    }
}

} // namespace realmheart::ui::bar::widgets
