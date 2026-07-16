#pragma once

#include <algorithm>
#include <gtk/gtk.h>

namespace realmheart::ui::bar::widgets {

// The anchor widgets are centred inside the 56 px rail, leaving roughly
// 11 px between their right edge and the rail contour. The attached popover
// moves 7 px into that existing gutter and uses a 4 px shell inset, so its body
// lands on the rail edge without ever covering the originating pill or rune.
inline constexpr int kAttachedPopoverOffsetX = 7;

// Media/system panels keep a two-pixel paint-only gutter in front of their
// visible shell. The popover allocation still reaches inward far enough to mask
// the bar rail, while the actual gold contour and content remain at the original
// seven-pixel visual position.
inline constexpr int kExpandingPopoverOffsetX = 5;
inline constexpr int kExpandingPopoverRevealPixels = kAttachedPopoverOffsetX;

namespace detail {

constexpr int kAttachedBodyInset = 4;
constexpr int kAttachedContentPadding = 10;
constexpr int kExpandingBodyInset = 0;
constexpr int kExpandingRailMaskWidth = 2;
constexpr int kExpandingContentPadding = 12;
constexpr int kExpandingCurveHeight = 22;

inline double attachment_half_height(GtkWidget* area, double shell_height) {
    GtkWidget* popover = gtk_widget_get_ancestor(area, GTK_TYPE_POPOVER);
    GtkWidget* anchor = popover != nullptr ? gtk_widget_get_parent(popover) : nullptr;
    const int anchor_height = anchor != nullptr ? gtk_widget_get_height(anchor) : 0;

    const double desired = anchor_height > 0
        ? static_cast<double>(anchor_height) / 2.0 + 3.0
        : 20.0;
    const double available = std::max(16.0, shell_height / 2.0 - 17.0);
    return std::clamp(desired, 16.0, std::min(48.0, available));
}

inline void shell_path(
    cairo_t* cr,
    double width,
    double height,
    double attachment_half,
    double body_x,
    double shoulder_depth,
    bool close_left_edge
) {
    constexpr double kInset = 1.5;

    const double right = std::max(body_x + 2.0, width - kInset);
    const double bottom = std::max(kInset + 2.0, height - kInset);
    const double center_y = height / 2.0;
    const double radius = std::clamp((height - 2.0 * kInset) * 0.11, 10.0, 14.0);
    const double join_top = center_y - attachment_half;
    const double join_bottom = center_y + attachment_half;
    const double shoulder_top = std::max(
        kInset + radius + 1.0,
        join_top - shoulder_depth
    );
    const double shoulder_bottom = std::min(
        bottom - radius - 1.0,
        join_bottom + shoulder_depth
    );

    cairo_new_path(cr);

    // The centre remains open: the bar's own rail line is masked by fill and
    // the panel redraws the top/right/bottom outline around the expansion.
    cairo_move_to(cr, 0.0, join_top);
    cairo_curve_to(
        cr,
        1.0, join_top,
        body_x, join_top - 4.0,
        body_x, shoulder_top
    );

    cairo_line_to(cr, body_x, kInset + radius);
    cairo_curve_to(
        cr,
        body_x, kInset,
        body_x + radius, kInset,
        body_x + radius, kInset
    );
    cairo_line_to(cr, right - radius, kInset);
    cairo_curve_to(
        cr,
        right, kInset,
        right, kInset + radius,
        right, kInset + radius
    );
    cairo_line_to(cr, right, bottom - radius);
    cairo_curve_to(
        cr,
        right, bottom,
        right - radius, bottom,
        right - radius, bottom
    );
    cairo_line_to(cr, body_x + radius, bottom);
    cairo_curve_to(
        cr,
        body_x, bottom,
        body_x, bottom - radius,
        body_x, bottom - radius
    );

    cairo_line_to(cr, body_x, shoulder_bottom);
    cairo_curve_to(
        cr,
        body_x, join_bottom + 4.0,
        1.0, join_bottom,
        0.0, join_bottom
    );

    if (close_left_edge) cairo_close_path(cr);
}

inline void attached_shell_path(
    cairo_t* cr,
    double width,
    double height,
    double attachment_half,
    bool close_left_edge
) {
    shell_path(
        cr,
        width,
        height,
        attachment_half,
        static_cast<double>(kAttachedBodyInset),
        10.0,
        close_left_edge
    );
}

inline void expanding_shell_path(
    cairo_t* cr,
    double width,
    double height,
    double /*attachment_half*/,
    bool close_left_edge
) {
    constexpr double kInset = 1.5;
    constexpr double kRailJoinX = static_cast<double>(kExpandingRailMaskWidth);
    constexpr double kCurveReach = 18.0 + static_cast<double>(kExpandingRailMaskWidth);

    const double right = std::max(kCurveReach + 2.0, width - kInset);
    const double rail_top = kInset;
    const double rail_bottom = std::max(kInset + 2.0, height - kInset);
    const double body_top = std::min(
        rail_bottom / 2.0,
        rail_top + static_cast<double>(kExpandingCurveHeight)
    );
    const double body_bottom = std::max(
        rail_bottom / 2.0,
        rail_bottom - static_cast<double>(kExpandingCurveHeight)
    );
    const double radius = std::clamp((body_bottom - body_top) * 0.11, 10.0, 14.0);
    const double curve_start_x = std::min(kCurveReach, std::max(kRailJoinX, right - radius));

    cairo_new_path(cr);

    // The shoulder needs real vertical room outside the panel body. Starting
    // below body_top creates an inward hook; starting above it lets the rail
    // bend down and outward into the panel as one continuous outer contour.
    cairo_move_to(cr, kRailJoinX, rail_top);
    cairo_curve_to(
        cr,
        kRailJoinX,
        rail_top + (static_cast<double>(kExpandingCurveHeight) * 0.56),
        curve_start_x * 0.42,
        body_top,
        curve_start_x,
        body_top
    );
    cairo_line_to(cr, right - radius, body_top);
    cairo_curve_to(
        cr,
        right,
        body_top,
        right,
        body_top + radius,
        right,
        body_top + radius
    );
    cairo_line_to(cr, right, body_bottom - radius);
    cairo_curve_to(
        cr,
        right,
        body_bottom,
        right - radius,
        body_bottom,
        right - radius,
        body_bottom
    );
    cairo_line_to(cr, curve_start_x, body_bottom);
    cairo_curve_to(
        cr,
        curve_start_x * 0.42,
        body_bottom,
        kRailJoinX,
        rail_bottom - (static_cast<double>(kExpandingCurveHeight) * 0.56),
        kRailJoinX,
        rail_bottom
    );

    if (close_left_edge) cairo_close_path(cr);
}

inline GdkRGBA widget_color(GtkWidget* widget) {
    GdkRGBA color{};
    gtk_widget_get_color(widget, &color);
    return color;
}

inline void draw_attached_fill(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer
) {
    const GdkRGBA fill = widget_color(GTK_WIDGET(area));
    const double attachment_half = attachment_half_height(GTK_WIDGET(area), height);
    attached_shell_path(cr, width, height, attachment_half, true);
    gdk_cairo_set_source_rgba(cr, &fill);
    cairo_fill(cr);
}

inline void draw_attached_border(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer
) {
    const GdkRGBA gold = widget_color(GTK_WIDGET(area));
    const double attachment_half = attachment_half_height(GTK_WIDGET(area), height);

    attached_shell_path(cr, width, height, attachment_half, false);
    gdk_cairo_set_source_rgba(cr, &gold);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_stroke(cr);
}

inline void draw_expanding_fill(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer
) {
    const GdkRGBA fill = widget_color(GTK_WIDGET(area));
    const double attachment_half = attachment_half_height(GTK_WIDGET(area), height);
    expanding_shell_path(cr, width, height, attachment_half, true);
    gdk_cairo_set_source_rgba(cr, &fill);
    cairo_fill(cr);

    // Paint only the hidden overlap gutter over the taskbar rail. The visible
    // shell starts two pixels later, so its contour/content retain their exact
    // pre-mask alignment while this strip removes the internal gold divider.
    cairo_rectangle(
        cr,
        0.0,
        0.0,
        static_cast<double>(kExpandingRailMaskWidth) + 0.5,
        static_cast<double>(height)
    );
    cairo_fill(cr);
}

inline void draw_expanding_border(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer
) {
    const GdkRGBA gold = widget_color(GTK_WIDGET(area));
    const double attachment_half = attachment_half_height(GTK_WIDGET(area), height);
    expanding_shell_path(cr, width, height, attachment_half, false);
    gdk_cairo_set_source_rgba(cr, &gold);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
    cairo_stroke(cr);
}

} // namespace detail

inline void set_attached_popover_child(GtkPopover* popover, GtkWidget* content) {
    if (popover == nullptr || content == nullptr) return;

    gtk_popover_set_has_arrow(popover, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(popover), "realmheart-attached-popover");

    GtkWidget* shell = gtk_overlay_new();
    gtk_widget_add_css_class(shell, "realmheart-attached-popover-shell");

    GtkWidget* fill_layer = gtk_drawing_area_new();
    gtk_widget_add_css_class(fill_layer, "realmheart-attached-popover-fill");
    gtk_widget_set_can_target(fill_layer, FALSE);
    gtk_widget_set_hexpand(fill_layer, TRUE);
    gtk_widget_set_vexpand(fill_layer, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(fill_layer),
        detail::draw_attached_fill,
        nullptr,
        nullptr
    );
    gtk_overlay_set_child(GTK_OVERLAY(shell), fill_layer);

    GtkWidget* border_layer = gtk_drawing_area_new();
    gtk_widget_add_css_class(border_layer, "realmheart-attached-popover-border");
    gtk_widget_set_can_target(border_layer, FALSE);
    gtk_widget_set_hexpand(border_layer, TRUE);
    gtk_widget_set_vexpand(border_layer, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(border_layer),
        detail::draw_attached_border,
        nullptr,
        nullptr
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(shell), border_layer);

    GtkWidget* content_host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(content_host, "realmheart-attached-popover-content");
    gtk_widget_set_hexpand(content_host, TRUE);
    gtk_widget_set_vexpand(content_host, TRUE);
    gtk_widget_set_margin_start(
        content_host,
        detail::kAttachedBodyInset + detail::kAttachedContentPadding
    );
    gtk_widget_set_margin_end(content_host, detail::kAttachedContentPadding);
    gtk_widget_set_margin_top(content_host, detail::kAttachedContentPadding);
    gtk_widget_set_margin_bottom(content_host, detail::kAttachedContentPadding);
    gtk_box_append(GTK_BOX(content_host), content);

    gtk_overlay_add_overlay(GTK_OVERLAY(shell), content_host);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(shell), content_host, TRUE);
    gtk_popover_set_child(popover, shell);
}

inline void set_expanding_popover_child(GtkPopover* popover, GtkWidget* content) {
    if (popover == nullptr || content == nullptr) return;

    gtk_popover_set_has_arrow(popover, FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(popover), "background");
    gtk_widget_add_css_class(GTK_WIDGET(popover), "realmheart-attached-popover");
    gtk_widget_add_css_class(GTK_WIDGET(popover), "realmheart-expanding-popover");

    GtkWidget* shell = gtk_overlay_new();
    gtk_widget_add_css_class(shell, "realmheart-attached-popover-shell");
    gtk_widget_add_css_class(shell, "realmheart-expanding-popover-shell");

    GtkWidget* fill_layer = gtk_drawing_area_new();
    gtk_widget_add_css_class(fill_layer, "realmheart-attached-popover-fill");
    gtk_widget_set_can_target(fill_layer, FALSE);
    gtk_widget_set_hexpand(fill_layer, TRUE);
    gtk_widget_set_vexpand(fill_layer, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(fill_layer),
        detail::draw_expanding_fill,
        nullptr,
        nullptr
    );
    gtk_overlay_set_child(GTK_OVERLAY(shell), fill_layer);

    GtkWidget* border_layer = gtk_drawing_area_new();
    gtk_widget_add_css_class(border_layer, "realmheart-attached-popover-border");
    gtk_widget_set_can_target(border_layer, FALSE);
    gtk_widget_set_hexpand(border_layer, TRUE);
    gtk_widget_set_vexpand(border_layer, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(border_layer),
        detail::draw_expanding_border,
        nullptr,
        nullptr
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(shell), border_layer);

    GtkWidget* content_host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(content_host, "realmheart-attached-popover-content");
    gtk_widget_add_css_class(content_host, "realmheart-expanding-popover-content");
    gtk_widget_set_hexpand(content_host, TRUE);
    gtk_widget_set_vexpand(content_host, TRUE);
    gtk_widget_set_margin_start(
        content_host,
        detail::kExpandingRailMaskWidth
            + detail::kExpandingBodyInset
            + detail::kExpandingContentPadding
    );
    gtk_widget_set_margin_end(content_host, detail::kExpandingContentPadding);
    gtk_widget_set_margin_top(
        content_host,
        detail::kExpandingCurveHeight + detail::kExpandingContentPadding
    );
    gtk_widget_set_margin_bottom(
        content_host,
        detail::kExpandingCurveHeight + detail::kExpandingContentPadding
    );
    gtk_box_append(GTK_BOX(content_host), content);

    gtk_overlay_add_overlay(GTK_OVERLAY(shell), content_host);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(shell), content_host, TRUE);
    gtk_popover_set_child(popover, shell);
}

} // namespace realmheart::ui::bar::widgets
