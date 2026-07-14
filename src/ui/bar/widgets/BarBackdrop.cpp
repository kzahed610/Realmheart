#include "ui/bar/widgets/BarBackdrop.hpp"

#include <algorithm>

namespace realmheart::ui::bar::widgets {
namespace {

constexpr double kQuarterEllipseKappa = 0.5522847498307936;
constexpr double kContourWidth = 3.0;

struct BackdropState {
    GWeakRef window_ref{};
    int rail_width = 0;
    int visual_width = 0;
    int curve_height = 0;
};

void destroy_state(gpointer raw) {
    auto* state = static_cast<BackdropState*>(raw);
    g_weak_ref_clear(&state->window_ref);
    delete state;
}

struct Geometry {
    double rail = 0.0;
    double visual = 0.0;
    double curve = 0.0;
    double cap = 0.0;
};

Geometry geometry_for(const BackdropState& state, int width, int height) {
    const double visual = static_cast<double>(std::max(width, 0));
    const double rail = std::clamp(
        static_cast<double>(state.rail_width),
        0.0,
        visual
    );
    const double curve = std::clamp(
        static_cast<double>(state.curve_height),
        0.0,
        static_cast<double>(std::max(height, 0)) / 2.0
    );
    return Geometry{
        .rail = rail,
        .visual = visual,
        .curve = curve,
        .cap = std::max(visual - rail, 0.0),
    };
}

void append_straight_rail_path(cairo_t* cr, const Geometry& geometry, int height) {
    cairo_rectangle(
        cr,
        0.0,
        0.0,
        geometry.rail,
        static_cast<double>(height)
    );
}

void append_top_cap_path(cairo_t* cr, const Geometry& geometry) {
    if (geometry.cap <= 0.0 || geometry.curve <= 0.0) return;

    cairo_move_to(cr, geometry.rail, 0.0);
    cairo_line_to(cr, geometry.visual, 0.0);
    cairo_curve_to(
        cr,
        geometry.visual - (kQuarterEllipseKappa * geometry.cap),
        0.0,
        geometry.rail,
        geometry.curve - (kQuarterEllipseKappa * geometry.curve),
        geometry.rail,
        geometry.curve
    );
    cairo_close_path(cr);
}

void append_bottom_cap_path(cairo_t* cr, const Geometry& geometry, int height) {
    if (geometry.cap <= 0.0 || geometry.curve <= 0.0) return;

    const double bottom = static_cast<double>(height);
    cairo_move_to(cr, geometry.rail, bottom - geometry.curve);
    cairo_curve_to(
        cr,
        geometry.rail,
        bottom - geometry.curve + (kQuarterEllipseKappa * geometry.curve),
        geometry.visual - (kQuarterEllipseKappa * geometry.cap),
        bottom,
        geometry.visual,
        bottom
    );
    cairo_line_to(cr, geometry.rail, bottom);
    cairo_close_path(cr);
}

void append_fill_path(cairo_t* cr, const Geometry& geometry, int height) {
    // Paint the full-height straight rail plus only the top and bottom curved
    // caps. The rectangular extension between both caps remains untouched and
    // therefore transparent, so the bar visually hugs the adjacent window.
    append_straight_rail_path(cr, geometry, height);
    append_top_cap_path(cr, geometry);
    append_bottom_cap_path(cr, geometry, height);
}

void append_contour_path(cairo_t* cr, const Geometry& geometry, int height) {
    const double bottom = static_cast<double>(height);

    cairo_move_to(cr, geometry.visual, 0.0);
    cairo_curve_to(
        cr,
        geometry.visual - (kQuarterEllipseKappa * geometry.cap),
        0.0,
        geometry.rail,
        geometry.curve - (kQuarterEllipseKappa * geometry.curve),
        geometry.rail,
        geometry.curve
    );
    cairo_line_to(cr, geometry.rail, bottom - geometry.curve);
    cairo_curve_to(
        cr,
        geometry.rail,
        bottom - geometry.curve + (kQuarterEllipseKappa * geometry.curve),
        geometry.visual - (kQuarterEllipseKappa * geometry.cap),
        bottom,
        geometry.visual,
        bottom
    );
}

void draw_fill(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    const auto& state = *static_cast<BackdropState*>(raw);
    const auto geometry = geometry_for(state, width, height);

    GdkRGBA fill{};
    gtk_widget_get_color(GTK_WIDGET(area), &fill);
    cairo_set_source_rgba(cr, fill.red, fill.green, fill.blue, fill.alpha);
    append_fill_path(cr, geometry, height);
    cairo_fill(cr);
}

void draw_contour(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    const auto& state = *static_cast<BackdropState*>(raw);
    const auto geometry = geometry_for(state, width, height);

    GdkRGBA contour{};
    gtk_widget_get_color(GTK_WIDGET(area), &contour);
    cairo_set_source_rgba(cr, contour.red, contour.green, contour.blue, contour.alpha);
    cairo_set_line_width(cr, kContourWidth);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    append_contour_path(cr, geometry, height);
    cairo_stroke(cr);
}

void update_input_region(BackdropState& state, int width, int height) {
    GObject* object = static_cast<GObject*>(g_weak_ref_get(&state.window_ref));
    if (object == nullptr) return;

    auto* window = GTK_WINDOW(object);
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (surface == nullptr || width <= 0 || height <= 0) {
        g_object_unref(object);
        return;
    }

    const auto geometry = geometry_for(state, width, height);
    cairo_region_t* region = cairo_region_create();

    const cairo_rectangle_int_t rail_rectangle{
        .x = 0,
        .y = 0,
        .width = static_cast<int>(geometry.rail),
        .height = height,
    };
    cairo_region_union_rectangle(region, &rail_rectangle);

    // Only the straight rail accepts input. The outward curve area is purely
    // decorative and remains click-through to the application beneath it.

    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
    g_object_unref(object);
}

void on_resize(GtkDrawingArea*, int width, int height, gpointer raw) {
    update_input_region(*static_cast<BackdropState*>(raw), width, height);
}

} // namespace

BarBackdrop::BarBackdrop(
    GtkWindow* window,
    int rail_width,
    int visual_width,
    int curve_height
) {
    auto* state = new BackdropState{
        .rail_width = std::max(rail_width, 0),
        .visual_width = std::max(visual_width, 0),
        .curve_height = std::max(curve_height, 0),
    };
    g_weak_ref_init(&state->window_ref, G_OBJECT(window));

    widget_ = gtk_overlay_new();
    gtk_widget_add_css_class(widget_, "realmheart-bar-backdrop");
    gtk_widget_set_size_request(widget_, state->visual_width, -1);
    gtk_widget_set_hexpand(widget_, TRUE);
    gtk_widget_set_vexpand(widget_, TRUE);
    g_object_set_data_full(
        G_OBJECT(widget_),
        "realmheart-bar-backdrop-state",
        state,
        destroy_state
    );

    GtkWidget* fill = gtk_drawing_area_new();
    gtk_widget_add_css_class(fill, "realmheart-bar-backdrop-fill");
    gtk_widget_set_can_target(fill, FALSE);
    gtk_widget_set_hexpand(fill, TRUE);
    gtk_widget_set_vexpand(fill, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(fill), draw_fill, state, nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(widget_), fill);

    GtkWidget* contour = gtk_drawing_area_new();
    gtk_widget_add_css_class(contour, "realmheart-bar-backdrop-contour");
    gtk_widget_set_can_target(contour, FALSE);
    gtk_widget_set_hexpand(contour, TRUE);
    gtk_widget_set_vexpand(contour, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(contour), draw_contour, state, nullptr);
    gtk_overlay_add_overlay(GTK_OVERLAY(widget_), contour);

    // DrawingArea::resize fires once after realization and whenever the
    // monitor allocation changes, keeping the click-through region in sync.
    g_signal_connect(fill, "resize", G_CALLBACK(on_resize), state);
}

} // namespace realmheart::ui::bar::widgets
