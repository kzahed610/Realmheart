#include "ui/powermenu/PowerMenuControls.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace realmheart::ui::powermenu {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool is_pointer_highlighted(GtkWidget* button) {
    const GtkStateFlags flags = gtk_widget_get_state_flags(button);
    return (flags & (GTK_STATE_FLAG_PRELIGHT | GTK_STATE_FLAG_ACTIVE)) != 0;
}

double mix(double start, double end, double amount) {
    return start + (end - start) * std::clamp(amount, 0.0, 1.0);
}

PowerMenuRect interpolate_rect(
    const PowerMenuRect& collapsed,
    const PowerMenuRect& expanded,
    double amount
) {
    return {
        mix(collapsed.x, expanded.x, amount),
        mix(collapsed.y, expanded.y, amount),
        mix(collapsed.width, expanded.width, amount),
        mix(collapsed.height, expanded.height, amount),
    };
}

void trace_button(
    cairo_t* cr,
    const PowerMenuRect& bounds,
    double scale,
    bool highlighted
) {
    const double point = std::min(
        (highlighted ? 40.0 : 31.0) * scale,
        bounds.height * (highlighted ? 0.35 : 0.315)
    );
    const double shoulder = point * (highlighted ? 0.42 : 0.46);
    const double shoulder_y = bounds.height * (highlighted ? 0.255 : 0.29);
    const double right = bounds.x + bounds.width;
    const double bottom = bounds.y + bounds.height;
    const double middle = bounds.y + bounds.height * 0.5;

    cairo_new_path(cr);
    cairo_move_to(cr, bounds.x, middle);
    cairo_line_to(cr, bounds.x + shoulder, bounds.y + shoulder_y);
    cairo_line_to(cr, bounds.x + point, bounds.y);
    cairo_line_to(cr, right - point, bounds.y);
    cairo_line_to(cr, right - shoulder, bounds.y + shoulder_y);
    cairo_line_to(cr, right, middle);
    cairo_line_to(cr, right - shoulder, bottom - shoulder_y);
    cairo_line_to(cr, right - point, bottom);
    cairo_line_to(cr, bounds.x + point, bottom);
    cairo_line_to(cr, bounds.x + shoulder, bottom - shoulder_y);
    cairo_close_path(cr);
}

PowerMenuRect inset(const PowerMenuRect& bounds, double amount) {
    return {
        bounds.x + amount,
        bounds.y + amount,
        std::max(0.0, bounds.width - amount * 2.0),
        std::max(0.0, bounds.height - amount * 2.0),
    };
}

void draw_diamond(cairo_t* cr, double x, double y, double radius) {
    cairo_new_path(cr);
    cairo_move_to(cr, x, y - radius);
    cairo_line_to(cr, x + radius, y);
    cairo_line_to(cr, x, y + radius);
    cairo_line_to(cr, x - radius, y);
    cairo_close_path(cr);
    cairo_fill(cr);
}

void stroke_diamond(cairo_t* cr, double x, double y, double radius) {
    cairo_new_path(cr);
    cairo_move_to(cr, x, y - radius);
    cairo_line_to(cr, x + radius, y);
    cairo_line_to(cr, x, y + radius);
    cairo_line_to(cr, x - radius, y);
    cairo_close_path(cr);
    cairo_stroke(cr);
}

void stroke_button(
    cairo_t* cr,
    const PowerMenuRect& bounds,
    double scale,
    bool highlighted,
    double width,
    double red,
    double green,
    double blue,
    double alpha
) {
    cairo_set_line_join(
        cr,
        width <= 2.5 ? CAIRO_LINE_JOIN_MITER : CAIRO_LINE_JOIN_ROUND
    );
    if (width <= 2.5) cairo_set_miter_limit(cr, 8.0);
    trace_button(cr, bounds, scale, highlighted);
    cairo_set_line_width(cr, width * scale);
    cairo_set_source_rgba(cr, red, green, blue, alpha);
    cairo_stroke(cr);
}

void draw_faceted_surface(
    cairo_t* cr,
    const PowerMenuRect& bounds,
    double scale,
    bool highlighted,
    bool pressed,
    bool armed
) {
    const double right = bounds.x + bounds.width;
    const double bottom = bounds.y + bounds.height;
    const double center_y = bounds.y + bounds.height * 0.5;
    const double point = std::min(
        (highlighted ? 40.0 : 31.0) * scale,
        bounds.height * (highlighted ? 0.35 : 0.315)
    );

    cairo_save(cr);
    trace_button(cr, bounds, scale, highlighted);
    cairo_clip(cr);

    cairo_pattern_t* enamel = cairo_pattern_create_linear(0.0, bounds.y, 0.0, bottom);
    const double warmth = armed ? 0.008 : 0.0;
    cairo_pattern_add_color_stop_rgba(
        enamel, 0.0,
        0.025 + warmth, 0.032 + warmth * 0.5, highlighted ? 0.060 : 0.052,
        pressed ? 0.995 : 0.975
    );
    cairo_pattern_add_color_stop_rgba(
        enamel, 0.40,
        0.010 + warmth * 0.4, 0.014, highlighted ? 0.030 : 0.027,
        0.985
    );
    cairo_pattern_add_color_stop_rgba(
        enamel, 1.0,
        0.004, 0.006, highlighted ? 0.014 : 0.017,
        0.995
    );
    cairo_set_source(cr, enamel);
    cairo_paint(cr);
    cairo_pattern_destroy(enamel);

    // Asymmetric, barely-visible enamel facets. They should reward a second look,
    // not announce a perfect procedural X through the center of every button.
    cairo_new_path(cr);
    cairo_move_to(cr, bounds.x + point, bounds.y);
    cairo_line_to(cr, bounds.x + bounds.width * 0.38, bounds.y);
    cairo_line_to(cr, bounds.x + bounds.width * 0.29, center_y * 0.96 + bounds.y * 0.04);
    cairo_line_to(cr, bounds.x + bounds.width * 0.07, center_y);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.075, 0.100, 0.165, highlighted ? 0.115 : 0.080);
    cairo_fill(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, bounds.x + bounds.width * 0.38, bounds.y);
    cairo_line_to(cr, right - point * 1.6, bounds.y);
    cairo_line_to(cr, bounds.x + bounds.width * 0.68, center_y * 1.03 - bounds.y * 0.03);
    cairo_line_to(cr, bounds.x + bounds.width * 0.29, center_y * 0.96 + bounds.y * 0.04);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.010, 0.014, 0.030, highlighted ? 0.22 : 0.16);
    cairo_fill(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, bounds.x + bounds.width * 0.07, center_y);
    cairo_line_to(cr, bounds.x + bounds.width * 0.29, center_y * 0.96 + bounds.y * 0.04);
    cairo_line_to(cr, bounds.x + bounds.width * 0.48, bottom);
    cairo_line_to(cr, bounds.x + point * 1.35, bottom);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.030, 0.046, 0.090, highlighted ? 0.115 : 0.075);
    cairo_fill(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, bounds.x + bounds.width * 0.29, center_y * 0.96 + bounds.y * 0.04);
    cairo_line_to(cr, bounds.x + bounds.width * 0.68, center_y * 1.03 - bounds.y * 0.03);
    cairo_line_to(cr, right - point * 1.2, bottom);
    cairo_line_to(cr, bounds.x + bounds.width * 0.48, bottom);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.075, 0.032, 0.105, highlighted ? 0.060 : 0.034);
    cairo_fill(cr);

    // A restrained top enamel sheen gives the panel depth without glassmorphism.
    cairo_pattern_t* sheen = cairo_pattern_create_linear(0.0, bounds.y, 0.0, center_y);
    cairo_pattern_add_color_stop_rgba(sheen, 0.0, 0.34, 0.39, 0.55, highlighted ? 0.055 : 0.030);
    cairo_pattern_add_color_stop_rgba(sheen, 1.0, 0.34, 0.39, 0.55, 0.0);
    cairo_rectangle(cr, bounds.x, bounds.y, bounds.width, bounds.height * 0.52);
    cairo_set_source(cr, sheen);
    cairo_fill(cr);
    cairo_pattern_destroy(sheen);

    cairo_restore(cr);
}

void draw_spine_connectors(
    cairo_t* cr,
    const std::array<PowerMenuRect, 5>& bounds,
    double scale
) {
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (std::size_t index = 0; index + 1 < bounds.size(); ++index) {
        const double center_x = bounds[index].x + bounds[index].width * 0.5;
        const double start_y = bounds[index].y + bounds[index].height + 5.0 * scale;
        const double end_y = bounds[index + 1].y - 5.0 * scale;
        if (end_y <= start_y) continue;

        cairo_set_line_width(cr, 4.5 * scale);
        cairo_set_source_rgba(cr, 0.46, 0.18, 0.70, 0.038);
        cairo_move_to(cr, center_x, start_y);
        cairo_line_to(cr, center_x, end_y);
        cairo_stroke(cr);

        cairo_set_line_width(cr, 0.72 * scale);
        cairo_set_source_rgba(cr, 0.76, 0.59, 0.37, 0.50);
        cairo_move_to(cr, center_x, start_y);
        cairo_line_to(cr, center_x, end_y);
        cairo_stroke(cr);

        const double middle_y = (start_y + end_y) * 0.5;
        cairo_set_source_rgba(cr, 0.20, 0.055, 0.34, 0.94);
        draw_diamond(cr, center_x, middle_y, 2.45 * scale);
        cairo_set_line_width(cr, 0.62 * scale);
        cairo_set_source_rgba(cr, 0.91, 0.72, 0.44, 0.78);
        stroke_diamond(cr, center_x, middle_y, 2.45 * scale);
        cairo_set_source_rgba(cr, 0.92, 0.77, 1.0, 0.74);
        draw_diamond(cr, center_x, middle_y, 0.65 * scale);
    }
    cairo_restore(cr);
}

void draw_center_ornaments(
    cairo_t* cr,
    const PowerMenuRect& bounds,
    double scale,
    double highlight_amount
) {
    const double amount = std::clamp(highlight_amount, 0.0, 1.0);
    const double reveal = amount * amount * (3.0 - 2.0 * amount);
    const double center_x = bounds.x + bounds.width * 0.5;
    const double radius = mix(10.2, 15.0, reveal) * scale;
    const double span = mix(39.0, 58.0, reveal) * scale;

    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_miter_limit(cr, 8.0);

    for (const int side : {-1, 1}) {
        const double y = side < 0 ? bounds.y : bounds.y + bounds.height;
        const double direction = static_cast<double>(side);

        // The base sigil grows and brightens continuously with the button.
        cairo_set_line_width(cr, mix(5.5, 11.0, reveal) * scale);
        cairo_set_source_rgba(cr, 0.92, 0.47, 0.16, mix(0.035, 0.095, reveal));
        cairo_move_to(cr, center_x - span, y);
        cairo_line_to(cr, center_x + span, y);
        cairo_stroke(cr);

        cairo_set_line_width(cr, mix(0.92, 1.35, reveal) * scale);
        cairo_set_source_rgba(cr, 0.92, 0.72, 0.44, mix(0.78, 1.0, reveal));
        cairo_move_to(cr, center_x - span, y);
        cairo_line_to(cr, center_x - radius * 2.25, y);
        cairo_line_to(cr, center_x - radius * 1.34, y + direction * radius * 0.48);
        cairo_line_to(cr, center_x - radius * 0.72, y + direction * radius * 0.48);
        cairo_line_to(cr, center_x, y + direction * radius * 1.18);
        cairo_line_to(cr, center_x + radius * 0.72, y + direction * radius * 0.48);
        cairo_line_to(cr, center_x + radius * 1.34, y + direction * radius * 0.48);
        cairo_line_to(cr, center_x + radius * 2.25, y);
        cairo_line_to(cr, center_x + span, y);
        cairo_stroke(cr);

        // Highlight-only filigree now grows out and fades in instead of popping in.
        if (reveal > 0.001) {
            const double crown_height = radius * mix(0.70, 1.88, reveal);
            cairo_set_line_width(cr, mix(0.45, 0.95, reveal) * scale);
            cairo_set_source_rgba(cr, 1.0, 0.84, 0.54, 0.90 * reveal);
            cairo_move_to(cr, center_x - radius * mix(1.50, 2.05, reveal), y);
            cairo_line_to(
                cr,
                center_x - radius * 1.10,
                y + direction * crown_height * 0.54
            );
            cairo_line_to(cr, center_x, y + direction * crown_height);
            cairo_line_to(
                cr,
                center_x + radius * 1.10,
                y + direction * crown_height * 0.54
            );
            cairo_line_to(cr, center_x + radius * mix(1.50, 2.05, reveal), y);
            cairo_stroke(cr);

            cairo_set_line_width(cr, mix(2.0, 10.0, reveal) * scale);
            cairo_set_source_rgba(cr, 1.0, 0.64, 0.20, 0.095 * reveal);
            cairo_move_to(
                cr,
                center_x,
                y + direction * radius * mix(0.90, 0.55, reveal)
            );
            cairo_line_to(
                cr,
                center_x,
                y + direction * radius * mix(1.10, 2.75, reveal)
            );
            cairo_stroke(cr);

            cairo_set_line_width(cr, mix(0.45, 1.05, reveal) * scale);
            cairo_set_source_rgba(cr, 0.99, 0.81, 0.50, 0.92 * reveal);
            cairo_move_to(cr, center_x, y + direction * radius * 1.20);
            cairo_line_to(
                cr,
                center_x,
                y + direction * radius * mix(1.35, 2.65, reveal)
            );
            cairo_stroke(cr);
        }

        cairo_set_source_rgba(cr, 0.012, 0.010, 0.026, 0.995);
        draw_diamond(cr, center_x, y, radius * 0.94);
        cairo_set_line_width(cr, mix(1.06, 1.55, reveal) * scale);
        cairo_set_source_rgba(cr, 0.98, 0.79, 0.49, mix(0.90, 1.0, reveal));
        stroke_diamond(cr, center_x, y, radius * 0.90);

        cairo_set_source_rgba(cr, 0.24, 0.052, 0.40, mix(0.90, 0.98, reveal));
        draw_diamond(cr, center_x, y, radius * 0.43);
        cairo_set_line_width(cr, 0.60 * scale);
        cairo_set_source_rgba(cr, 0.78, 0.52, 0.95, mix(0.80, 0.98, reveal));
        stroke_diamond(cr, center_x, y, radius * 0.43);
        cairo_set_source_rgba(cr, 1.0, 0.88, 0.62, mix(0.82, 1.0, reveal));
        draw_diamond(cr, center_x, y, radius * 0.12);

        for (const int wing : {-1, 1}) {
            const double wing_x = center_x + wing * radius * 1.48;
            cairo_set_source_rgba(cr, 0.018, 0.012, 0.030, 0.99);
            draw_diamond(cr, wing_x, y, radius * 0.19);
            cairo_set_line_width(cr, 0.52 * scale);
            cairo_set_source_rgba(cr, 0.92, 0.71, 0.43, mix(0.68, 0.92, reveal));
            stroke_diamond(cr, wing_x, y, radius * 0.19);
        }
    }
    cairo_restore(cr);
}

void draw_side_ornaments(
    cairo_t* cr,
    const PowerMenuRect& bounds,
    double scale,
    double highlight_amount
) {
    const double amount = std::clamp(highlight_amount, 0.0, 1.0);
    const double reveal = amount * amount * (3.0 - 2.0 * amount);
    const double middle_y = bounds.y + bounds.height * 0.5;
    const double center_x = bounds.x + bounds.width * 0.5;
    const double radius = mix(7.2, 10.2, reveal) * scale;
    const double inset_x = mix(20.0, 25.0, reveal) * scale;

    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_miter_limit(cr, 8.0);

    // Side flares extend outward and brighten continuously with hover progress.
    if (reveal > 0.001) {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
        for (const int side : {-1, 1}) {
            const double edge = side < 0 ? bounds.x : bounds.x + bounds.width;
            const double direction = static_cast<double>(side);
            const double start = edge + direction * 4.0 * scale;

            cairo_set_line_width(cr, mix(2.0, 12.0, reveal) * scale);
            cairo_set_source_rgba(cr, 1.0, 0.58, 0.18, 0.045 * reveal);
            cairo_move_to(cr, start, middle_y);
            cairo_line_to(
                cr,
                edge + direction * mix(10.0, 72.0, reveal) * scale,
                middle_y
            );
            cairo_stroke(cr);

            cairo_set_line_width(cr, mix(0.8, 4.0, reveal) * scale);
            cairo_set_source_rgba(cr, 1.0, 0.74, 0.30, 0.16 * reveal);
            cairo_move_to(cr, start, middle_y);
            cairo_line_to(
                cr,
                edge + direction * mix(8.0, 62.0, reveal) * scale,
                middle_y
            );
            cairo_stroke(cr);

            cairo_set_line_width(cr, mix(0.35, 1.15, reveal) * scale);
            cairo_set_source_rgba(cr, 1.0, 0.88, 0.58, 0.92 * reveal);
            cairo_move_to(cr, start, middle_y);
            cairo_line_to(
                cr,
                edge + direction * mix(6.0, 54.0, reveal) * scale,
                middle_y
            );
            cairo_stroke(cr);
        }
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    }

    for (const double x : {
        bounds.x + inset_x,
        bounds.x + bounds.width - inset_x,
    }) {
        const double inward = x < center_x ? 1.0 : -1.0;

        cairo_set_line_width(cr, mix(4.2, 9.0, reveal) * scale);
        cairo_set_source_rgba(cr, 0.93, 0.48, 0.16, mix(0.032, 0.105, reveal));
        stroke_diamond(cr, x, middle_y, radius * 1.20);

        cairo_set_source_rgba(cr, 0.012, 0.010, 0.026, 0.995);
        draw_diamond(cr, x, middle_y, radius * 1.04);
        cairo_set_line_width(cr, mix(1.00, 1.45, reveal) * scale);
        cairo_set_source_rgba(cr, 0.98, 0.78, 0.47, mix(0.86, 1.0, reveal));
        stroke_diamond(cr, x, middle_y, radius);

        cairo_set_source_rgba(cr, 0.25, 0.052, 0.42, mix(0.86, 0.98, reveal));
        draw_diamond(cr, x, middle_y, radius * 0.42);
        cairo_set_line_width(cr, 0.50 * scale);
        cairo_set_source_rgba(cr, 0.79, 0.53, 0.96, mix(0.72, 0.95, reveal));
        stroke_diamond(cr, x, middle_y, radius * 0.42);
        cairo_set_source_rgba(cr, 1.0, 0.88, 0.60, mix(0.80, 1.0, reveal));
        draw_diamond(cr, x, middle_y, radius * 0.12);

        // The etched stems lengthen with the same progress as the button.
        cairo_set_line_width(cr, mix(0.72, 0.94, reveal) * scale);
        cairo_set_source_rgba(cr, 0.94, 0.74, 0.46, mix(0.62, 0.88, reveal));
        cairo_move_to(cr, x + inward * radius * 1.36, middle_y);
        cairo_line_to(
            cr,
            x + inward * radius * mix(1.72, 1.92, reveal),
            middle_y
        );
        cairo_stroke(cr);

        cairo_move_to(cr, x - inward * radius * 1.30, middle_y);
        cairo_line_to(
            cr,
            x - inward * radius * mix(1.58, 1.82, reveal),
            middle_y
        );
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void draw_lock(cairo_t* cr, double cx, double cy, double size) {
    cairo_save(cr);
    cairo_set_line_width(cr, size * 0.075);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy - size * 0.095, size * 0.245, kPi, 2.0 * kPi);
    cairo_stroke(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, cx - size * 0.285, cy - size * 0.045);
    cairo_line_to(cr, cx + size * 0.285, cy - size * 0.045);
    cairo_line_to(cr, cx + size * 0.285, cy + size * 0.395);
    cairo_line_to(cr, cx - size * 0.285, cy + size * 0.395);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.025, 0.025, 0.040, 0.98);
    cairo_arc(cr, cx, cy + size * 0.145, size * 0.046, 0.0, 2.0 * kPi);
    cairo_fill(cr);
    cairo_rectangle(cr, cx - size * 0.020, cy + size * 0.145, size * 0.040, size * 0.115);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_suspend(cairo_t* cr, double cx, double cy, double size) {
    cairo_save(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, size * 0.385, 0.0, 2.0 * kPi);
    cairo_arc(cr, cx + size * 0.165, cy - size * 0.115, size * 0.315, 0.0, 2.0 * kPi);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_logout(cairo_t* cr, double cx, double cy, double size) {
    cairo_arc(cr, cx, cy - size * 0.225, size * 0.145, 0.0, 2.0 * kPi);
    cairo_fill(cr);

    cairo_new_path(cr);
    cairo_move_to(cr, cx - size * 0.32, cy + size * 0.34);
    cairo_curve_to(
        cr,
        cx - size * 0.30, cy + size * 0.045,
        cx + size * 0.30, cy + size * 0.045,
        cx + size * 0.32, cy + size * 0.34
    );
    cairo_line_to(cr, cx + size * 0.18, cy + size * 0.34);
    cairo_curve_to(
        cr,
        cx + size * 0.12, cy + size * 0.18,
        cx - size * 0.12, cy + size * 0.18,
        cx - size * 0.18, cy + size * 0.34
    );
    cairo_close_path(cr);
    cairo_fill(cr);
}

void draw_reboot(cairo_t* cr, double cx, double cy, double size) {
    const double radius = size * 0.34;
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, radius, -0.30 * kPi, 1.48 * kPi);
    cairo_stroke(cr);

    const double x = cx + radius * std::cos(-0.30 * kPi);
    const double y = cy + radius * std::sin(-0.30 * kPi);
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x - size * 0.025, y + size * 0.225);
    cairo_line_to(cr, x - size * 0.205, y + size * 0.078);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_arc(cr, cx, cy, size * 0.035, 0.0, 2.0 * kPi);
    cairo_fill(cr);
}

void draw_power(cairo_t* cr, double cx, double cy, double size) {
    cairo_move_to(cr, cx, cy - size * 0.46);
    cairo_line_to(cr, cx, cy - size * 0.015);
    cairo_stroke(cr);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy + size * 0.035, size * 0.365, -0.24 * kPi, 1.24 * kPi);
    cairo_stroke(cr);
}

void draw_icon(
    cairo_t* cr,
    PowerMenuAction action,
    const PowerMenuRect& bounds,
    double scale,
    bool highlighted,
    bool emphasized
) {
    const double size = (highlighted ? 36.0 : 32.0) * scale;
    const double cx = bounds.x + bounds.width - (highlighted ? 64.0 : 58.0) * scale;
    const double cy = bounds.y + bounds.height * 0.5;

    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, (highlighted ? 2.40 : 2.05) * scale);
    cairo_set_source_rgba(cr, 0.985, 0.975, 0.94, emphasized ? 1.0 : 0.93);
    switch (action) {
        case PowerMenuAction::Lock: draw_lock(cr, cx, cy, size); break;
        case PowerMenuAction::Suspend: draw_suspend(cr, cx, cy, size); break;
        case PowerMenuAction::Logout: draw_logout(cr, cx, cy, size); break;
        case PowerMenuAction::Reboot: draw_reboot(cr, cx, cy, size); break;
        case PowerMenuAction::PowerOff: draw_power(cr, cx, cy, size); break;
    }
    cairo_restore(cr);
}

void draw_label(
    cairo_t* cr,
    std::string_view text,
    const PowerMenuRect& bounds,
    double scale,
    bool highlighted,
    bool emphasized
) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font = pango_font_description_new();
    pango_font_description_set_family(font, "Noto Serif Display");
    pango_font_description_set_weight(font, PANGO_WEIGHT_MEDIUM);
    pango_font_description_set_absolute_size(
        font,
        (highlighted ? 34.2 : 30.2) * scale * PANGO_SCALE
    );
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
    pango_layout_set_single_paragraph_mode(layout, TRUE);

    PangoAttrList* attributes = pango_attr_list_new();
    pango_attr_list_insert(
        attributes,
        pango_attr_letter_spacing_new(
            static_cast<int>((highlighted ? 5.1 : 4.5) * scale * PANGO_SCALE)
        )
    );
    pango_layout_set_attributes(layout, attributes);

    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    const double x = bounds.x + (bounds.width - text_width) * 0.5;
    const double y = bounds.y + (bounds.height - text_height) * 0.5 - 0.5 * scale;

    // Tight shadow and a restrained warm halo, rather than enlarging the text excessively.
    cairo_move_to(cr, x + 0.8 * scale, y + 1.2 * scale);
    cairo_set_source_rgba(cr, 0.006, 0.004, 0.012, highlighted ? 0.94 : 0.78);
    pango_cairo_show_layout(cr, layout);

    if (highlighted) {
        for (const double offset : {-1.0, 1.0}) {
            cairo_move_to(cr, x + offset * scale, y);
            cairo_set_source_rgba(cr, 1.0, 0.76, 0.34, 0.115);
            pango_cairo_show_layout(cr, layout);
        }
    }

    cairo_move_to(cr, x, y);
    cairo_set_source_rgba(
        cr,
        highlighted ? 1.0 : 0.955,
        highlighted ? 0.965 : 0.945,
        highlighted ? 0.845 : 0.915,
        emphasized ? 1.0 : 0.965
    );
    pango_cairo_show_layout(cr, layout);

    pango_attr_list_unref(attributes);
    pango_font_description_free(font);
    g_object_unref(layout);
}

void draw_button(
    cairo_t* cr,
    const PowerMenuButtonLayout& button,
    double scale,
    double highlight_amount,
    bool hovered,
    bool pressed,
    bool focused,
    bool armed
) {
    const double amount = std::clamp(highlight_amount, 0.0, 1.0);
    const bool hot = hovered || pressed;
    const bool highlighted = amount >= 0.40 || hot || armed;
    const bool emphasized = amount >= 0.22 || armed;
    const auto& bounds = button.bounds;

    cairo_save(cr);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    // Ambient aura smoothly ramps in instead of snapping on hover.
    stroke_button(
        cr, bounds, scale, highlighted,
        mix(7.0, hot ? 26.0 : 20.0, amount),
        mix(0.95, 1.0, amount),
        mix(0.60, armed ? 0.38 : 0.60, amount),
        mix(0.20, armed ? 0.11 : 0.18, amount),
        mix(0.020, hot ? 0.105 : (armed ? 0.072 : 0.055), amount)
    );
    stroke_button(
        cr, bounds, scale, highlighted,
        mix(3.0, hot ? 14.0 : 10.0, amount),
        mix(0.84, 1.0, amount),
        mix(0.57, armed ? 0.56 : 0.77, amount),
        mix(0.28, armed ? 0.17 : 0.30, amount),
        mix(0.042, hot ? 0.195 : (armed ? 0.130 : 0.100), amount)
    );
    stroke_button(
        cr, bounds, scale, highlighted,
        mix(1.6, hot ? 6.5 : 4.5, amount),
        1.0,
        mix(0.70, hot ? 0.86 : 0.80, amount),
        mix(0.30, hot ? 0.44 : 0.40, amount),
        mix(0.055, hot ? 0.36 : (armed ? 0.26 : 0.20), amount)
    );

    draw_faceted_surface(cr, bounds, scale, highlighted, pressed, armed);

    // Layered metallic frame: sharp gold, dark separator, gold/violet inner hairlines.
    stroke_button(
        cr, bounds, scale, highlighted,
        mix(1.30, 1.80, amount),
        mix(0.71, 1.0, amount),
        mix(0.56, 0.82, amount),
        mix(0.34, 0.50, amount),
        mix(0.94, 1.0, amount)
    );

    stroke_button(
        cr, inset(bounds, 2.35 * scale), scale, highlighted,
        1.35,
        0.005, 0.006, 0.013,
        0.94
    );

    stroke_button(
        cr, inset(bounds, 4.35 * scale), scale, highlighted,
        mix(0.70, 0.92, amount),
        mix(0.76, 0.98, amount),
        mix(0.62, 0.83, amount),
        mix(0.43, 0.60, amount),
        mix(0.50, 0.80, amount)
    );

    stroke_button(
        cr, inset(bounds, 6.65 * scale), scale, highlighted,
        0.48,
        0.63, 0.38, 0.78,
        mix(0.18, 0.31, amount)
    );

    if (focused && amount < 0.18) {
        stroke_button(
            cr, inset(bounds, 5.25 * scale), scale, false,
            0.55,
            0.73, 0.49, 0.92,
            0.16
        );
    }

    draw_side_ornaments(cr, bounds, scale, amount);
    draw_center_ornaments(cr, bounds, scale, amount);
    draw_label(cr, button.label, bounds, scale, highlighted, emphasized);
    draw_icon(cr, button.action, bounds, scale, highlighted, emphasized);
    cairo_restore(cr);
}

} // namespace

PowerMenuControls::PowerMenuControls(ActionCallback on_action, DismissCallback on_dismiss)
    : on_action_(std::move(on_action)),
      on_dismiss_(std::move(on_dismiss)) {
    root_ = gtk_overlay_new();
    gtk_widget_set_cursor_from_name(root_, "default");
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);

    canvas_ = gtk_drawing_area_new();
    gtk_widget_set_cursor_from_name(canvas_, "default");
    gtk_widget_set_hexpand(canvas_, TRUE);
    gtk_widget_set_vexpand(canvas_, TRUE);
    gtk_widget_set_can_target(canvas_, FALSE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(canvas_),
        +[](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
            static_cast<PowerMenuControls*>(data)->draw(cr, width, height);
        },
        this,
        nullptr
    );
    gtk_overlay_set_child(GTK_OVERLAY(root_), canvas_);

    button_layer_ = gtk_fixed_new();
    gtk_widget_set_cursor_from_name(button_layer_, "default");
    gtk_widget_set_hexpand(button_layer_, TRUE);
    gtk_widget_set_vexpand(button_layer_, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), button_layer_);

    const auto& definitions = power_menu_buttons();
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto& definition = definitions[index];
        GtkWidget* button = gtk_button_new();
        buttons_[index] = button;
        gtk_widget_add_css_class(button, "realmheart-power-action");
        gtk_widget_set_cursor_from_name(button, "default");
        gtk_accessible_update_property(
            GTK_ACCESSIBLE(button),
            GTK_ACCESSIBLE_PROPERTY_LABEL,
            definition.label.data(),
            GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
            "Activate once to arm, then activate again to confirm",
            -1
        );
        g_object_set_data(
            G_OBJECT(button),
            "realmheart-power-action",
            GINT_TO_POINTER(static_cast<int>(definition.action) + 1)
        );
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
            auto* self = static_cast<PowerMenuControls*>(data);
            const int stored = GPOINTER_TO_INT(
                g_object_get_data(G_OBJECT(clicked), "realmheart-power-action")
            );
            if (self->on_action_) self->on_action_(static_cast<Action>(stored - 1));
        }), this);
        g_signal_connect(button, "state-flags-changed", G_CALLBACK(+[](
            GtkWidget*, GtkStateFlags, gpointer data
        ) {
            static_cast<PowerMenuControls*>(data)->update_layout();
        }), this);
        gtk_fixed_put(GTK_FIXED(button_layer_), button, 0.0, 0.0);
    }

    const auto resize = +[](GObject*, GParamSpec*, gpointer data) {
        static_cast<PowerMenuControls*>(data)->update_layout();
    };
    g_signal_connect(button_layer_, "notify::width", G_CALLBACK(resize), this);
    g_signal_connect(button_layer_, "notify::height", G_CALLBACK(resize), this);

    GtkGesture* outside_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(outside_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(outside_click, "released", G_CALLBACK(+[](
        GtkGestureClick*, int, double x, double y, gpointer data
    ) {
        auto* self = static_cast<PowerMenuControls*>(data);
        if (!self->contains_action(x, y) && self->on_dismiss_) self->on_dismiss_();
    }), this);
    gtk_widget_add_controller(root_, GTK_EVENT_CONTROLLER(outside_click));

    if (GdkDisplay* display = gdk_display_get_default(); display != nullptr) {
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, R"CSS(
            .realmheart-power-action,
            .realmheart-power-action:hover,
            .realmheart-power-action:active,
            .realmheart-power-action:focus {
                min-width: 0; min-height: 0; padding: 0; margin: 0;
                border: none; border-radius: 0; outline: none; box-shadow: none;
                background: transparent; background-image: none;
            }
        )CSS");
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
        g_object_unref(provider);
    }
}

GtkWidget* PowerMenuControls::widget() const {
    return root_;
}

void PowerMenuControls::focus_first() {
    if (prepare_for_interaction()) return;

    // A first mapped tick can still precede the layer-shell configure/allocation.
    // Keep retrying until the real fullscreen size exists instead of consuming a
    // single zero-sized frame and leaving the invisible hit targets at 0,0.
    gtk_widget_add_tick_callback(
        button_layer_,
        +[](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
            auto* self = static_cast<PowerMenuControls*>(data);
            return self->prepare_for_interaction()
                ? G_SOURCE_REMOVE
                : G_SOURCE_CONTINUE;
        },
        this,
        nullptr
    );
}

bool PowerMenuControls::prepare_for_interaction() {
    if (gtk_widget_get_width(button_layer_) <= 0 ||
        gtk_widget_get_height(button_layer_) <= 0) {
        return false;
    }

    update_layout();
    const bool action_has_focus = std::any_of(
        buttons_.begin(),
        buttons_.end(),
        [](GtkWidget* button) {
            return button != nullptr && gtk_widget_has_focus(button);
        }
    );
    if (!action_has_focus && buttons_[0] != nullptr) {
        gtk_widget_grab_focus(buttons_[0]);
    }
    return true;
}

void PowerMenuControls::sync_animation_targets() {
    if (button_layer_ == nullptr) return;

    const int width = gtk_widget_get_width(button_layer_);
    const int height = gtk_widget_get_height(button_layer_);
    if (width <= 0 || height <= 0) return;

    const PowerMenuLayout layout = power_menu_layout(width, height);
    bool needs_animation = false;
    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        const bool active = is_pointer_highlighted(buttons_[index]) ||
            (armed_action_.has_value() && *armed_action_ == layout.buttons[index].action);
        animation_target_[index] = active ? 1.0 : 0.0;
        if (std::abs(animation_target_[index] - animation_progress_[index]) > 0.001) {
            needs_animation = true;
        }
    }
    if (needs_animation) ensure_animation_tick();
}

void PowerMenuControls::ensure_animation_tick() {
    if (animation_tick_id_ != 0 || root_ == nullptr) return;
    animation_tick_id_ = gtk_widget_add_tick_callback(
        root_,
        +[](GtkWidget*, GdkFrameClock* frame_clock, gpointer data) -> gboolean {
            auto* self = static_cast<PowerMenuControls*>(data);
            if (self->advance_animations(frame_clock)) {
                return G_SOURCE_CONTINUE;
            }
            self->animation_tick_id_ = 0;
            self->last_animation_frame_time_ = 0;
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

bool PowerMenuControls::advance_animations(GdkFrameClock* frame_clock) {
    sync_animation_targets();

    const gint64 frame_time = gdk_frame_clock_get_frame_time(frame_clock);
    const double delta_seconds = last_animation_frame_time_ == 0
        ? (1.0 / 60.0)
        : std::clamp(
            static_cast<double>(frame_time - last_animation_frame_time_) / 1000000.0,
            1.0 / 240.0,
            1.0 / 20.0
        );
    last_animation_frame_time_ = frame_time;

    const double response = 1.0 - std::exp(-delta_seconds * 18.0);
    bool animating = false;
    for (std::size_t index = 0; index < animation_progress_.size(); ++index) {
        animation_progress_[index] +=
            (animation_target_[index] - animation_progress_[index]) * response;
        if (std::abs(animation_target_[index] - animation_progress_[index]) <= 0.0015) {
            animation_progress_[index] = animation_target_[index];
        } else {
            animating = true;
        }
    }

    update_layout();
    return animating;
}

double PowerMenuControls::animation_amount(std::size_t index) const {
    return std::clamp(animation_progress_[index], 0.0, 1.0);
}

void PowerMenuControls::set_armed(std::optional<Action> action) {
    armed_action_ = action;
    sync_animation_targets();
    update_layout();
}

void PowerMenuControls::update_layout() {
    const int width = gtk_widget_get_width(button_layer_);
    const int height = gtk_widget_get_height(button_layer_);
    if (width <= 0 || height <= 0) return;

    sync_animation_targets();

    const PowerMenuLayout layout = power_menu_layout(width, height);
    for (std::size_t index = 0; index < buttons_.size(); ++index) {
        const auto collapsed = power_menu_button_bounds(layout.buttons[index], false);
        const auto expanded = power_menu_button_bounds(layout.buttons[index], true);
        const auto bounds = interpolate_rect(
            collapsed,
            expanded,
            animation_amount(index)
        );
        gtk_widget_set_size_request(
            buttons_[index],
            static_cast<int>(std::ceil(bounds.width)),
            static_cast<int>(std::ceil(bounds.height))
        );
        gtk_fixed_move(GTK_FIXED(button_layer_), buttons_[index], bounds.x, bounds.y);
    }
    queue_draw();
}

void PowerMenuControls::draw(cairo_t* cr, int width, int height) const {
    if (width <= 0 || height <= 0) return;
    const PowerMenuLayout layout = power_menu_layout(width, height);

    std::array<PowerMenuRect, 5> visual_bounds{};
    for (std::size_t index = 0; index < layout.buttons.size(); ++index) {
        const auto collapsed = power_menu_button_bounds(layout.buttons[index], false);
        const auto expanded = power_menu_button_bounds(layout.buttons[index], true);
        visual_bounds[index] = interpolate_rect(
            collapsed,
            expanded,
            animation_amount(index)
        );
    }
    draw_spine_connectors(cr, visual_bounds, layout.scale);

    const auto render = [&](std::size_t index) {
        const GtkStateFlags flags = gtk_widget_get_state_flags(buttons_[index]);
        const bool hovered = (flags & GTK_STATE_FLAG_PRELIGHT) != 0;
        const bool pressed = (flags & GTK_STATE_FLAG_ACTIVE) != 0;
        const bool armed = armed_action_.has_value() &&
            *armed_action_ == layout.buttons[index].action;
        PowerMenuButtonLayout visual = layout.buttons[index];
        visual.bounds = visual_bounds[index];
        draw_button(
            cr,
            visual,
            layout.scale,
            animation_amount(index),
            hovered,
            pressed,
            gtk_widget_has_focus(buttons_[index]),
            armed
        );
    };

    const auto is_visually_emphasized = [&](std::size_t index) {
        return animation_amount(index) > 0.001;
    };
    for (std::size_t index = 0; index < layout.buttons.size(); ++index) {
        if (!is_visually_emphasized(index)) render(index);
    }
    for (std::size_t index = 0; index < layout.buttons.size(); ++index) {
        if (is_visually_emphasized(index)) render(index);
    }
}

void PowerMenuControls::queue_draw() {
    if (canvas_ != nullptr) gtk_widget_queue_draw(canvas_);
}

bool PowerMenuControls::contains_action(double x, double y) const {
    const int width = gtk_widget_get_width(root_);
    const int height = gtk_widget_get_height(root_);
    if (width <= 0 || height <= 0) return false;
    const PowerMenuLayout layout = power_menu_layout(width, height);
    for (std::size_t index = 0; index < layout.buttons.size(); ++index) {
        const auto collapsed = power_menu_button_bounds(layout.buttons[index], false);
        const auto expanded = power_menu_button_bounds(layout.buttons[index], true);
        const auto bounds = interpolate_rect(
            collapsed,
            expanded,
            animation_amount(index)
        );
        if (x >= bounds.x && x <= bounds.x + bounds.width &&
            y >= bounds.y && y <= bounds.y + bounds.height) {
            return true;
        }
    }
    return false;
}

} // namespace realmheart::ui::powermenu
