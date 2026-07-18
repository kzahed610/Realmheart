#include "ui/sidebar/SidebarFrame.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace realmheart::ui::sidebar {
namespace {

constexpr double kOuterPathInset = 10.0;
constexpr double kMiddleFillInset = 12.0;
constexpr double kInnerFillInset = 16.0;
constexpr double kInnerContourInset = 19.0;
constexpr double kHighlightContourInset = 22.0;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct FrameGeometry {
    double left = 0.0;
    double right = 0.0;
    double top = 0.0;
    double bottom = 0.0;

    double header_left = 0.0;
    double body_left = 0.0;
    double lower_left = 0.0;
    double header_end = 0.0;
    double first_spine_end = 0.0;
    double second_spine_start = 0.0;
    double second_spine_end = 0.0;
    double lower_step_start = 0.0;

    double bottom_center = 0.0;
};

struct FrameState {
    GWeakRef window_ref{};
    SidebarFrameLayout layout{};
};

void destroy_state(gpointer raw) {
    auto* state = static_cast<FrameState*>(raw);
    g_weak_ref_clear(&state->window_ref);
    delete state;
}

FrameGeometry frame_geometry(
    int width,
    int height,
    double inset,
    const SidebarFrameLayout& layout
) {
    const double surface_width = std::max(static_cast<double>(width), 1.0);
    const double frame_width = std::min(
        static_cast<double>(layout.frame_width),
        surface_width
    );
    const double frame_origin = std::max(surface_width - frame_width, 0.0);

    FrameGeometry geometry;
    geometry.left = frame_origin + inset;
    geometry.right = std::max(
        geometry.left + 1.0,
        surface_width - inset
    );

    // Leave explicit crown/finial breathing room inside the allocation. This
    // means detail can protrude above and below the main body without clipping.
    geometry.top = inset + 12.0;
    geometry.bottom = std::max(
        geometry.top + 1.0,
        // Keep enough room below the finial for the soft, downward-cast shadow
        // to fade inside the transparent layer-shell surface instead of being
        // clipped into a hard horizontal edge.
        static_cast<double>(height) - inset - 30.0
    );

    const double usable_width = geometry.right - geometry.left;
    const double usable_height = geometry.bottom - geometry.top;

    // The upper chamber stays deliberately narrower than the body. The open
    // upper-left shoulder and transparent gutter are the future Tessia entry
    // zone, so ornaments frame that region rather than occupying it.
    geometry.header_left = geometry.left +
        std::clamp(usable_width * 0.185, 68.0, 82.0);
    geometry.body_left = geometry.left +
        std::clamp(usable_width * 0.095, 34.0, 43.0);
    geometry.lower_left = geometry.left +
        std::clamp(usable_width * 0.040, 14.0, 19.0);
    geometry.header_end = geometry.top + (usable_height * 0.220);
    geometry.first_spine_end = geometry.top + (usable_height * 0.350);
    geometry.second_spine_start = geometry.top + (usable_height * 0.590);
    geometry.second_spine_end = geometry.top + (usable_height * 0.715);
    geometry.lower_step_start = geometry.bottom -
        std::clamp(usable_height * 0.135, 82.0, 108.0);

    geometry.bottom_center = geometry.left + (usable_width * 0.565);
    return geometry;
}

std::vector<Point> silhouette_points(
    int width,
    int height,
    double inset,
    const SidebarFrameLayout& layout
) {
    const auto g = frame_geometry(width, height, inset, layout);

    // The reference treats the right border as a narrow structural spine rather
    // than the same-width bevel used on every side. Pull only the inner layers
    // farther inward so the outer gold remains thin while a restrained dark
    // frame band gains depth. The outer silhouette and input region stay at
    // their original width because kOuterPathInset produces zero recess.
    const double right_spine_recess = std::clamp(
        (inset - kMiddleFillInset) * 1.35,
        0.0,
        9.0
    );
    const double right_edge = g.right - right_spine_recess;

    const double lower_transition_y = std::min(
        g.second_spine_end + 23.0,
        g.lower_step_start - 18.0
    );
    const double lower_transition_inner_y = std::min(
        g.second_spine_end + 8.0,
        lower_transition_y - 15.0
    );
    const double second_spine_bulge_y = std::min(
        g.second_spine_start + 27.0,
        lower_transition_inner_y - 1.0
    );

    // Clockwise, beginning on the stepped top-left shoulder. The extra breaks
    // are structural rather than ornamental: even with every inner decoration
    // removed, the surface still reads as the reference's forged artefact.
    return {
        {g.header_left + 34.0, g.top},
        {g.header_left + 46.0, g.top - 6.0},

        // Reinforced top-right cap. The reference lets the crown project a few
        // pixels farther than the long side wall, then settles into a quiet,
        // nearly vertical spine. This keeps the right edge architectural
        // without reintroducing a rune rail or detached decorative strokes.
        {right_edge - 112.0, g.top - 6.0},
        {right_edge - 96.0, g.top - 17.0},
        {right_edge - 52.0, g.top - 17.0},
        {right_edge - 39.0, g.top - 8.0},
        {right_edge - 14.0, g.top - 8.0},
        {right_edge, g.top + 7.0},
        {right_edge, g.top + 25.0},
        {right_edge - 7.0, g.top + 33.0},

        // The central right run stays deliberately flat. Near the foundation,
        // a shallow two-stage foot projects outward again, echoing the
        // reference's lower power-section transition without tying the shell
        // to any particular future inner layout.
        {right_edge - 7.0, g.lower_step_start - 28.0},
        {right_edge - 2.0, g.lower_step_start - 18.0},
        {right_edge - 2.0, g.bottom - 43.0},
        {right_edge - 19.0, g.bottom - 20.0},
        {right_edge - 37.0, g.bottom},

        {g.bottom_center + 42.0, g.bottom},
        {g.bottom_center + 29.0, g.bottom + 5.0},
        {g.bottom_center + 16.0, g.bottom + 5.0},
        {g.bottom_center + 8.0, g.bottom + 14.0},
        {g.bottom_center, g.bottom + 21.0},
        {g.bottom_center - 8.0, g.bottom + 14.0},
        {g.bottom_center - 16.0, g.bottom + 5.0},
        {g.bottom_center - 29.0, g.bottom + 5.0},
        {g.header_left - 9.0, g.bottom + 5.0},
        {g.body_left + 15.0, g.bottom - 6.0},
        {g.lower_left, g.bottom - 27.0},
        {g.lower_left, g.bottom - 71.0},
        {g.body_left - 8.0, g.bottom - 84.0},
        {g.body_left - 19.0, g.lower_step_start - 4.0},
        {g.body_left - 19.0, lower_transition_y},
        {g.body_left - 29.0, lower_transition_inner_y},
        {g.body_left - 29.0, second_spine_bulge_y},

        // Keep the lower forged irregularities, but remove the upper protruding
        // spine entirely. The widened, nearly flat wall gives Tessia's cropped
        // facial edge a clean occluding surface instead of fighting her pose.
        {g.body_left, g.second_spine_start + 8.0},
        {g.body_left, g.first_spine_end + 68.0},
        {g.body_left - 10.0, g.first_spine_end + 50.0},
        {g.body_left - 10.0, g.top + 96.0},
        {g.body_left - 2.0, g.top + 72.0},
        {g.header_left - 15.0, g.top + 55.0},
        {g.header_left + 4.0, g.top + 45.0},
        {g.header_left + 14.0, g.top + 28.0},
        {g.header_left + 27.0, g.top + 20.0},
    };
}

void append_polygon(cairo_t* cr, const std::vector<Point>& points) {
    if (points.empty()) return;

    cairo_move_to(cr, points.front().x, points.front().y);
    for (std::size_t index = 1; index < points.size(); ++index) {
        cairo_line_to(cr, points[index].x, points[index].y);
    }
    cairo_close_path(cr);
}

GdkRGBA widget_colour(GtkWidget* widget) {
    GdkRGBA colour{};
    gtk_widget_get_color(widget, &colour);
    return colour;
}

void set_source_from_widget(
    GtkWidget* widget,
    cairo_t* cr,
    double alpha_scale = 1.0
) {
    const auto colour = widget_colour(widget);
    cairo_set_source_rgba(
        cr,
        colour.red,
        colour.green,
        colour.blue,
        std::clamp(colour.alpha * alpha_scale, 0.0, 1.0)
    );
}

void fill_vertical_gradient(
    GtkWidget* widget,
    cairo_t* cr,
    double top,
    double bottom,
    double top_alpha,
    double middle_alpha,
    double bottom_alpha
) {
    const auto colour = widget_colour(widget);
    cairo_pattern_t* gradient = cairo_pattern_create_linear(0.0, top, 0.0, bottom);
    cairo_pattern_add_color_stop_rgba(
        gradient,
        0.0,
        colour.red,
        colour.green,
        colour.blue,
        colour.alpha * top_alpha
    );
    cairo_pattern_add_color_stop_rgba(
        gradient,
        0.48,
        colour.red,
        colour.green,
        colour.blue,
        colour.alpha * middle_alpha
    );
    cairo_pattern_add_color_stop_rgba(
        gradient,
        1.0,
        colour.red,
        colour.green,
        colour.blue,
        colour.alpha * bottom_alpha
    );
    cairo_set_source(cr, gradient);
    cairo_pattern_destroy(gradient);
}

void draw_outer_shadow(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    const auto& state = *static_cast<FrameState*>(raw);
    const auto points = silhouette_points(
        width,
        height,
        kOuterPathInset,
        state.layout
    );

    // The old shadow was three centred, increasingly dark outlines. That read
    // as a thick black border rather than depth. Cast the shadow slightly left
    // and down instead: a low-alpha contact silhouette provides separation,
    // while two broad outlines provide a restrained falloff over the wallpaper.
    cairo_save(cr);
    cairo_translate(cr, -4.0, 5.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    append_polygon(cr, points);
    set_source_from_widget(GTK_WIDGET(area), cr, 0.045);
    cairo_set_line_width(cr, 28.0);
    cairo_stroke(cr);

    append_polygon(cr, points);
    set_source_from_widget(GTK_WIDGET(area), cr, 0.085);
    cairo_set_line_width(cr, 15.0);
    cairo_stroke(cr);

    append_polygon(cr, points);
    set_source_from_widget(GTK_WIDGET(area), cr, 0.20);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_fill_at(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    FrameState& state,
    double inset,
    double top_alpha,
    double middle_alpha,
    double bottom_alpha
) {
    const auto g = frame_geometry(width, height, inset, state.layout);
    append_polygon(cr, silhouette_points(width, height, inset, state.layout));
    fill_vertical_gradient(
        GTK_WIDGET(area),
        cr,
        g.top - 12.0,
        g.bottom + 22.0,
        top_alpha,
        middle_alpha,
        bottom_alpha
    );
    cairo_fill(cr);
}

void draw_outer_fill(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    draw_fill_at(
        area,
        cr,
        width,
        height,
        *static_cast<FrameState*>(raw),
        kOuterPathInset,
        1.0,
        0.88,
        0.98
    );
}

void draw_middle_fill(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    draw_fill_at(
        area,
        cr,
        width,
        height,
        *static_cast<FrameState*>(raw),
        kMiddleFillInset,
        0.98,
        0.80,
        0.94
    );
}

void draw_inner_fill(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    draw_fill_at(
        area,
        cr,
        width,
        height,
        *static_cast<FrameState*>(raw),
        kInnerFillInset,
        0.98,
        0.93,
        1.0
    );
}

void stroke_silhouette(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    FrameState& state,
    double inset,
    double line_width,
    double alpha = 1.0
) {
    append_polygon(cr, silhouette_points(width, height, inset, state.layout));
    set_source_from_widget(GTK_WIDGET(area), cr, alpha);
    cairo_set_line_width(cr, line_width);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
    cairo_stroke(cr);
}

void draw_outer_contour(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    auto& state = *static_cast<FrameState*>(raw);
    stroke_silhouette(
        area,
        cr,
        width,
        height,
        state,
        kOuterPathInset,
        1.55
    );
}

void draw_inner_contour(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    auto& state = *static_cast<FrameState*>(raw);
    stroke_silhouette(
        area,
        cr,
        width,
        height,
        state,
        kInnerContourInset,
        0.85
    );
}

void draw_highlight_contour(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    auto& state = *static_cast<FrameState*>(raw);
    stroke_silhouette(
        area,
        cr,
        width,
        height,
        state,
        kHighlightContourInset,
        0.45,
        0.72
    );
}

void draw_gold_details(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    const auto& state = *static_cast<FrameState*>(raw);
    const auto g = frame_geometry(
        width,
        height,
        kHighlightContourInset,
        state.layout
    );

    set_source_from_widget(GTK_WIDGET(area), cr);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
    cairo_set_line_width(cr, 0.85);

    // Keep the upper frame intentionally clean. Its silhouette and nested
    // contours already define the crown; extra short strokes read as detached
    // decoration and compete with both the title and Tessia's future peek zone.
    // The bottom finial remains the frame's only standalone gold ornament.
    cairo_move_to(cr, g.bottom_center, g.bottom - 8.0);
    cairo_line_to(cr, g.bottom_center + 12.0, g.bottom + 7.0);
    cairo_line_to(cr, g.bottom_center, g.bottom + 19.0);
    cairo_line_to(cr, g.bottom_center - 12.0, g.bottom + 7.0);
    cairo_close_path(cr);
    cairo_stroke(cr);

    cairo_move_to(cr, g.bottom_center, g.bottom - 2.0);
    cairo_line_to(cr, g.bottom_center + 6.0, g.bottom + 7.0);
    cairo_line_to(cr, g.bottom_center, g.bottom + 14.0);
    cairo_line_to(cr, g.bottom_center - 6.0, g.bottom + 7.0);
    cairo_close_path(cr);
    cairo_stroke(cr);
}

void draw_violet_details(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    const auto& state = *static_cast<FrameState*>(raw);
    const auto g = frame_geometry(
        width,
        height,
        kHighlightContourInset,
        state.layout
    );

    // Keep violet as a tiny material accent in the bottom finial only. The
    // right edge deliberately has no rune rail or glyphs; its visual weight
    // now comes from the uninterrupted layered contours.
    set_source_from_widget(GTK_WIDGET(area), cr, 0.94);
    cairo_move_to(cr, g.bottom_center, g.bottom + 1.0);
    cairo_line_to(cr, g.bottom_center + 4.0, g.bottom + 7.0);
    cairo_line_to(cr, g.bottom_center, g.bottom + 12.0);
    cairo_line_to(cr, g.bottom_center - 4.0, g.bottom + 7.0);
    cairo_close_path(cr);
    cairo_fill(cr);
}

std::vector<double> scanline_intersections(
    const std::vector<Point>& points,
    double scan_y
) {
    std::vector<double> intersections;
    intersections.reserve(8);

    for (std::size_t index = 0; index < points.size(); ++index) {
        const Point& first = points[index];
        const Point& second = points[(index + 1) % points.size()];
        const bool crosses = (first.y <= scan_y && second.y > scan_y) ||
            (second.y <= scan_y && first.y > scan_y);
        if (!crosses) continue;

        const double fraction = (scan_y - first.y) / (second.y - first.y);
        intersections.push_back(first.x + ((second.x - first.x) * fraction));
    }

    std::sort(intersections.begin(), intersections.end());
    return intersections;
}

void update_input_region(FrameState& state, int width, int height) {
    GObject* object = static_cast<GObject*>(g_weak_ref_get(&state.window_ref));
    if (object == nullptr) return;

    auto* window = GTK_WINDOW(object);
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (surface == nullptr || width <= 0 || height <= 0) {
        g_object_unref(object);
        return;
    }

    const auto points = silhouette_points(
        width,
        height,
        kOuterPathInset,
        state.layout
    );
    cairo_region_t* region = cairo_region_create();

    int active_x = -1;
    int active_width = 0;
    int active_start_y = 0;

    const auto flush_run = [&](int end_y) {
        if (active_x < 0 || active_width <= 0 || end_y <= active_start_y) return;
        const cairo_rectangle_int_t rectangle{
            .x = active_x,
            .y = active_start_y,
            .width = active_width,
            .height = end_y - active_start_y,
        };
        cairo_region_union_rectangle(region, &rectangle);
    };

    for (int y = 0; y < height; ++y) {
        const auto intersections = scanline_intersections(
            points,
            static_cast<double>(y) + 0.5
        );
        int row_x = -1;
        int row_width = 0;

        if (intersections.size() >= 2) {
            row_x = std::clamp(
                static_cast<int>(std::floor(intersections.front())),
                0,
                width
            );
            const int row_end = std::clamp(
                static_cast<int>(std::ceil(intersections.back())),
                0,
                width
            );
            row_width = std::max(row_end - row_x, 0);
        }

        if (row_x == active_x && row_width == active_width) continue;
        flush_run(y);
        active_x = row_x;
        active_width = row_width;
        active_start_y = y;
    }
    flush_run(height);

    // Character art remains decorative and click-through by default. When a
    // future interaction is added, its region can be unioned here independently
    // without changing the shell silhouette.
    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
    g_object_unref(object);
}

void on_resize(GtkDrawingArea*, int width, int height, gpointer raw) {
    update_input_region(*static_cast<FrameState*>(raw), width, height);
}

GtkWidget* drawing_layer(
    const char* css_class,
    GtkDrawingAreaDrawFunc draw,
    FrameState* state
) {
    GtkWidget* layer = gtk_drawing_area_new();
    gtk_widget_add_css_class(layer, css_class);
    gtk_widget_set_can_target(layer, FALSE);
    gtk_widget_set_hexpand(layer, TRUE);
    gtk_widget_set_vexpand(layer, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(layer),
        draw,
        state,
        nullptr
    );
    return layer;
}

GtkWidget* art_layer(const char* css_class) {
    GtkWidget* layer = gtk_fixed_new();
    gtk_widget_add_css_class(layer, css_class);
    gtk_widget_set_can_target(layer, FALSE);
    gtk_widget_set_hexpand(layer, TRUE);
    gtk_widget_set_vexpand(layer, TRUE);
    return layer;
}

} // namespace

SidebarFrame::SidebarFrame(GtkWindow* window, SidebarFrameLayout layout)
    : layout_(layout) {
    auto* state = new FrameState{};
    state->layout = layout_;
    g_weak_ref_init(&state->window_ref, G_OBJECT(window));

    root_ = gtk_overlay_new();
    gtk_widget_add_css_class(root_, "realmheart-sidebar-frame");
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);
    g_object_set_data_full(
        G_OBJECT(root_),
        "realmheart-sidebar-frame-state",
        state,
        destroy_state
    );

    GtkWidget* shadow = drawing_layer(
        "realmheart-sidebar-frame-shadow",
        draw_outer_shadow,
        state
    );
    gtk_overlay_set_child(GTK_OVERLAY(root_), shadow);

    back_art_layer_ = art_layer("realmheart-sidebar-back-art");
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), back_art_layer_);

    GtkWidget* outer_fill = drawing_layer(
        "realmheart-sidebar-frame-outer-fill",
        draw_outer_fill,
        state
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), outer_fill);

    GtkWidget* middle_fill = drawing_layer(
        "realmheart-sidebar-frame-middle-fill",
        draw_middle_fill,
        state
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), middle_fill);

    GtkWidget* inner_fill = drawing_layer(
        "realmheart-sidebar-frame-inner-fill",
        draw_inner_fill,
        state
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), inner_fill);

    GtkWidget* outer_contour = drawing_layer(
        "realmheart-sidebar-frame-outer-contour",
        draw_outer_contour,
        state
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), outer_contour);

    GtkWidget* inner_contour = drawing_layer(
        "realmheart-sidebar-frame-inner-contour",
        draw_inner_contour,
        state
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), inner_contour);

    GtkWidget* highlight_contour = drawing_layer(
        "realmheart-sidebar-frame-highlight-contour",
        draw_highlight_contour,
        state
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), highlight_contour);

    content_holder_ = gtk_overlay_new();
    gtk_widget_add_css_class(content_holder_, "realmheart-sidebar-content-layer");
    gtk_widget_set_hexpand(content_holder_, TRUE);
    gtk_widget_set_vexpand(content_holder_, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), content_holder_);

    front_art_layer_ = art_layer("realmheart-sidebar-front-art");
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), front_art_layer_);

    GtkWidget* gold_details = drawing_layer(
        "realmheart-sidebar-frame-gold-detail",
        draw_gold_details,
        state
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), gold_details);

    GtkWidget* violet_details = drawing_layer(
        "realmheart-sidebar-frame-violet-detail",
        draw_violet_details,
        state
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), violet_details);

    g_signal_connect(shadow, "resize", G_CALLBACK(on_resize), state);
}

void SidebarFrame::set_child(GtkWidget* child) {
    if (child == nullptr || content_ != nullptr) return;

    content_ = child;
    gtk_widget_set_hexpand(content_, TRUE);
    gtk_widget_set_vexpand(content_, TRUE);
    gtk_widget_set_halign(content_, GTK_ALIGN_FILL);
    gtk_widget_set_valign(content_, GTK_ALIGN_FILL);

    // Insets are relative to the visible frame, while the content widget lives
    // in the full surface. Adding frame_origin_x keeps today's controls in the
    // exact same on-screen position after introducing the transparent Tessia
    // gutter.
    gtk_widget_set_margin_start(
        content_,
        layout_.frame_origin_x() + layout_.content_inset_start
    );
    gtk_widget_set_margin_end(content_, layout_.content_inset_end);
    gtk_widget_set_margin_top(content_, layout_.content_inset_top);
    gtk_widget_set_margin_bottom(content_, layout_.content_inset_bottom);
    gtk_overlay_set_child(GTK_OVERLAY(content_holder_), content_);
}

} // namespace realmheart::ui::sidebar
