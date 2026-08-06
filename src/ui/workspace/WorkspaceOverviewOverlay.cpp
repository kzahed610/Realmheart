#include "ui/workspace/WorkspaceOverviewOverlay.hpp"

#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"

#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace realmheart::ui::workspace {
namespace {

constexpr double kReferenceWidth = 1920.0;
constexpr double kReferenceHeight = 1080.0;
constexpr double kActiveFraction = 0.56;
constexpr double kInactiveFraction = (1.0 - kActiveFraction) / 3.0;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Color {
    double red = 1.0;
    double green = 1.0;
    double blue = 1.0;
    double alpha = 1.0;
};

struct RealmStyle {
    std::string_view roman;
    std::string_view element;
    std::string_view place;
    std::string_view character;
    std::string_view background_asset;
    std::string_view character_asset;
    Color accent;
    Color accent_soft;
    double active_height = 760.0;
    double active_right = 0.0;
    double idle_height = 430.0;
    double idle_right = 0.0;
    double idle_top = -35.0;
};

constexpr std::array<RealmStyle, 4> kRealms{{
    {
        "I", "FIRE", "The Hearth", "Bairon Wykes",
        "workspace-overview/backgrounds/1080p/fire-the-hearth.png",
        "workspace-overview/characters/1080p/bairon-wykes.png",
        {1.0, 0.525, 0.271, 1.0},
        {1.0, 0.816, 0.604, 1.0},
        760.0, -28.0, 430.0, -5.0, -35.0,
    },
    {
        "II", "WATER", "Etistin Bay", "Varay Aurae",
        "workspace-overview/backgrounds/1080p/water-etistin-bay.png",
        "workspace-overview/characters/1080p/varay-aurae.png",
        {0.325, 0.784, 1.0, 1.0},
        {0.682, 0.914, 1.0, 1.0},
        780.0, -58.0, 430.0, -8.0, -36.0,
    },
    {
        "III", "WIND", "Elshire Forest", "Aya Grephin",
        "workspace-overview/backgrounds/1080p/wind-elshire-forest.png",
        "workspace-overview/characters/1080p/aya-grephin.png",
        {0.471, 0.843, 0.741, 1.0},
        {0.773, 0.957, 0.910, 1.0},
        760.0, 4.0, 420.0, 15.0, -33.0,
    },
    {
        "IV", "EARTH", "Vildorial", "Mica Earthborn",
        "workspace-overview/backgrounds/1080p/earth-vildorial.png",
        "workspace-overview/characters/1080p/mica-earthborn.png",
        {0.820, 0.639, 0.373, 1.0},
        {0.941, 0.831, 0.643, 1.0},
        760.0, -18.0, 430.0, -2.0, -35.0,
    },
}};

constexpr std::array<double, 13> kBoundaryXPercent{
    0.0, 8.0, 16.0, 24.0, 32.0, 40.0, 50.0,
    60.0, 70.0, 80.0, 88.0, 94.0, 100.0,
};

constexpr std::array<std::array<double, 13>, 3> kBoundaryOffsets{{
    {6.0, -18.0, -4.0, 14.0, -12.0, 8.0, -20.0, 4.0, 17.0, -10.0, 7.0, -5.0, 11.0},
    {0.0, 16.0, 28.0, 13.0, -8.0, -21.0, -5.0, 18.0, 27.0, 9.0, -15.0, -7.0, 2.0},
    {3.0, -12.0, 16.0, -7.0, 21.0, -17.0, 7.0, -14.0, 19.0, -5.0, 13.0, -9.0, 4.0},
}};

constexpr std::array<std::array<std::pair<std::string_view, std::string_view>, 3>, 4>
    kFakeWindows{{
        {{{"Zen Browser", "Realmheart · GitHub"}, {"kitty", "realmheart build"}, {"Spotify", "The Hearth Mix"}}},
        {{{"Notes", "Tomorrow plan"}, {"Music", "Siren's Call"}, {"Files", "workspace assets"}}},
        {{{"Editor", "WorkspaceOverview.cpp"}, {"Terminal", "hyprctl clients -j"}, {"Docs", "interaction notes"}}},
        {{{"Build", "cmake --build"}, {"Tests", "Workspace model"}, {"Monitor", "Realmheart RSS"}}},
    }};

void set_source(cairo_t* cr, const Color& color, double alpha_multiplier = 1.0) {
    cairo_set_source_rgba(
        cr,
        color.red,
        color.green,
        color.blue,
        std::clamp(color.alpha * alpha_multiplier, 0.0, 1.0)
    );
}

void rounded_rectangle(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height,
    double radius
) {
    const double r = std::min({radius, width * 0.5, height * 0.5});
    constexpr double k = 0.5522847498307936;
    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + r, y);
    cairo_line_to(cr, x + width - r, y);
    cairo_curve_to(
        cr,
        x + width - r + r * k, y,
        x + width, y + r - r * k,
        x + width, y + r
    );
    cairo_line_to(cr, x + width, y + height - r);
    cairo_curve_to(
        cr,
        x + width, y + height - r + r * k,
        x + width - r + r * k, y + height,
        x + width - r, y + height
    );
    cairo_line_to(cr, x + r, y + height);
    cairo_curve_to(
        cr,
        x + r - r * k, y + height,
        x, y + height - r + r * k,
        x, y + height - r
    );
    cairo_line_to(cr, x, y + r);
    cairo_curve_to(
        cr,
        x, y + r - r * k,
        x + r - r * k, y,
        x + r, y
    );
    cairo_close_path(cr);
}

void draw_text(
    cairo_t* cr,
    std::string_view text,
    double x,
    double y,
    double size,
    const Color& color,
    bool bold = false,
    std::string_view family = "Inter",
    bool align_right = false,
    int letter_spacing = 0
) {
    PangoLayout* layout = pango_cairo_create_layout(cr);
    const std::string owned(text);
    pango_layout_set_text(layout, owned.c_str(), -1);

    PangoFontDescription* font = pango_font_description_new();
    const std::string family_name(family);
    pango_font_description_set_family(font, family_name.c_str());
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(
        font,
        bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL
    );
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    if (letter_spacing != 0) {
        PangoAttrList* attributes = pango_attr_list_new();
        PangoAttribute* spacing = pango_attr_letter_spacing_new(
            letter_spacing * PANGO_SCALE
        );
        pango_attr_list_insert(attributes, spacing);
        pango_layout_set_attributes(layout, attributes);
        pango_attr_list_unref(attributes);
    }

    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    const double draw_x = align_right ? x - text_width : x;

    set_source(cr, color);
    cairo_move_to(cr, draw_x, y - static_cast<double>(text_height) * 0.5);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

std::array<double, 4> realm_heights(int active_index) {
    std::array<double, 4> heights{};
    for (int index = 0; index < 4; ++index) {
        heights[static_cast<std::size_t>(index)] =
            (index == active_index ? kActiveFraction : kInactiveFraction) *
            kReferenceHeight;
    }
    return heights;
}

std::array<double, 4> realm_tops(const std::array<double, 4>& heights) {
    std::array<double, 4> tops{};
    for (std::size_t index = 1; index < tops.size(); ++index) {
        tops[index] = tops[index - 1] + heights[index - 1];
    }
    return tops;
}

std::array<Point, 13> boundary_points(double base, std::size_t boundary_index) {
    std::array<Point, 13> points{};
    for (std::size_t index = 0; index < points.size(); ++index) {
        points[index] = {
            kBoundaryXPercent[index] * kReferenceWidth / 100.0,
            base + kBoundaryOffsets[boundary_index][index],
        };
    }
    return points;
}

std::array<Point, 13> shifted(
    const std::array<Point, 13>& points,
    double delta_y
) {
    auto result = points;
    for (auto& point : result) point.y += delta_y;
    return result;
}

std::array<Point, 13> flat_boundary(double y) {
    std::array<Point, 13> points{};
    for (std::size_t index = 0; index < points.size(); ++index) {
        points[index] = {
            kBoundaryXPercent[index] * kReferenceWidth / 100.0,
            y,
        };
    }
    return points;
}

void polygon_path(
    cairo_t* cr,
    const std::array<Point, 13>& top,
    const std::array<Point, 13>& bottom
) {
    cairo_new_path(cr);
    cairo_move_to(cr, top.front().x, top.front().y);
    for (std::size_t index = 1; index < top.size(); ++index) {
        cairo_line_to(cr, top[index].x, top[index].y);
    }
    for (auto index = bottom.size(); index-- > 0;) {
        cairo_line_to(cr, bottom[index].x, bottom[index].y);
    }
    cairo_close_path(cr);
}

void frontier_path(
    cairo_t* cr,
    const std::array<Point, 13>& points,
    double upward,
    double downward
) {
    cairo_new_path(cr);
    cairo_move_to(cr, points.front().x, points.front().y - upward);
    for (std::size_t index = 1; index < points.size(); ++index) {
        cairo_line_to(cr, points[index].x, points[index].y - upward);
    }
    for (auto index = points.size(); index-- > 0;) {
        cairo_line_to(cr, points[index].x, points[index].y + downward);
    }
    cairo_close_path(cr);
}

[[nodiscard]] cairo_surface_t* load_cairo_surface(
    const std::filesystem::path& path,
    std::string& error_message
) {
    GError* texture_error = nullptr;
    GdkTexture* texture = gdk_texture_new_from_filename(
        path.c_str(),
        &texture_error
    );
    if (texture == nullptr) {
        error_message = "Unable to load " + path.filename().string();
        if (texture_error != nullptr && texture_error->message != nullptr) {
            error_message += ": ";
            error_message += texture_error->message;
        }
        g_clear_error(&texture_error);
        return nullptr;
    }

    const int width = gdk_texture_get_width(texture);
    const int height = gdk_texture_get_height(texture);
    cairo_surface_t* surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32,
        width,
        height
    );
    const cairo_status_t status = cairo_surface_status(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        error_message = "Unable to create Cairo surface for " +
            path.filename().string() + ": " + cairo_status_to_string(status);
        cairo_surface_destroy(surface);
        g_object_unref(texture);
        return nullptr;
    }

    gdk_texture_download(
        texture,
        cairo_image_surface_get_data(surface),
        static_cast<gsize>(cairo_image_surface_get_stride(surface))
    );
    cairo_surface_mark_dirty(surface);
    g_object_unref(texture);
    return surface;
}

void draw_surface_cover(
    cairo_t* cr,
    cairo_surface_t* surface,
    double x,
    double y,
    double width,
    double height
) {
    if (surface == nullptr || width <= 0.0 || height <= 0.0) return;
    const int source_width = cairo_image_surface_get_width(surface);
    const int source_height = cairo_image_surface_get_height(surface);
    if (source_width <= 0 || source_height <= 0) return;

    const double scale = std::max(
        width / static_cast<double>(source_width),
        height / static_cast<double>(source_height)
    );
    const double drawn_width = static_cast<double>(source_width) * scale;
    const double drawn_height = static_cast<double>(source_height) * scale;
    const double draw_x = x + width - drawn_width;
    const double draw_y = y + (height - drawn_height) * 0.5;

    cairo_save(cr);
    cairo_rectangle(cr, x, y, width, height);
    cairo_clip(cr);
    cairo_translate(cr, draw_x, draw_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);
}

void draw_character(
    cairo_t* cr,
    cairo_surface_t* surface,
    double target_height,
    double right,
    double top
) {
    if (surface == nullptr || target_height <= 0.0) return;
    const int source_width = cairo_image_surface_get_width(surface);
    const int source_height = cairo_image_surface_get_height(surface);
    if (source_width <= 0 || source_height <= 0) return;

    const double scale = target_height / static_cast<double>(source_height);
    const double width = static_cast<double>(source_width) * scale;
    const double x = kReferenceWidth - right - width;

    cairo_save(cr);
    cairo_translate(cr, x, top);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);
}

void draw_atmosphere(
    cairo_t* cr,
    double top,
    double height,
    bool active
) {
    cairo_pattern_t* horizontal = cairo_pattern_create_linear(0.0, 0.0, kReferenceWidth, 0.0);
    cairo_pattern_add_color_stop_rgba(horizontal, 0.0, 0.01, 0.02, 0.03, 0.92);
    cairo_pattern_add_color_stop_rgba(horizontal, 0.15, 0.02, 0.03, 0.04, active ? 0.64 : 0.72);
    cairo_pattern_add_color_stop_rgba(horizontal, 0.45, 0.02, 0.03, 0.04, active ? 0.17 : 0.26);
    cairo_pattern_add_color_stop_rgba(horizontal, 0.75, 0.01, 0.02, 0.03, active ? 0.01 : 0.04);
    cairo_pattern_add_color_stop_rgba(horizontal, 1.0, 0.0, 0.0, 0.0, active ? 0.08 : 0.10);
    cairo_rectangle(cr, 0.0, top, kReferenceWidth, height);
    cairo_set_source(cr, horizontal);
    cairo_fill(cr);
    cairo_pattern_destroy(horizontal);

    cairo_pattern_t* vertical = cairo_pattern_create_linear(0.0, top, 0.0, top + height);
    cairo_pattern_add_color_stop_rgba(vertical, 0.0, 0.0, 0.0, 0.0, active ? 0.08 : 0.18);
    cairo_pattern_add_color_stop_rgba(vertical, 0.45, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(vertical, 1.0, 0.0, 0.0, 0.0, active ? 0.24 : 0.33);
    cairo_rectangle(cr, 0.0, top, kReferenceWidth, height);
    cairo_set_source(cr, vertical);
    cairo_fill(cr);
    cairo_pattern_destroy(vertical);
}

void draw_window_card(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height,
    const RealmStyle& style,
    std::pair<std::string_view, std::string_view> content,
    bool compact
) {
    rounded_rectangle(cr, x, y, width, height, compact ? 11.0 : 15.0);
    cairo_pattern_t* background = cairo_pattern_create_linear(x, y, x + width, y + height);
    cairo_pattern_add_color_stop_rgba(background, 0.0, 0.035, 0.047, 0.059, 0.94);
    cairo_pattern_add_color_stop_rgba(background, 1.0, 0.051, 0.067, 0.082, 0.84);
    cairo_set_source(cr, background);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(background);

    set_source(cr, style.accent, 0.34);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    const double titlebar_height = compact ? 34.0 : 38.0;
    set_source(cr, {0.0, 0.0, 0.0, 0.17});
    cairo_rectangle(cr, x, y, width, titlebar_height);
    cairo_fill(cr);

    set_source(cr, style.accent);
    cairo_arc(
        cr,
        x + 18.0,
        y + titlebar_height * 0.5,
        compact ? 4.2 : 5.0,
        0.0,
        2.0 * std::acos(-1.0)
    );
    cairo_fill(cr);

    draw_text(
        cr,
        content.first,
        x + 33.0,
        y + titlebar_height * 0.5,
        compact ? 12.0 : 14.0,
        {0.95, 0.94, 0.91, 0.96},
        true
    );
    draw_text(
        cr,
        content.second,
        x + width - 14.0,
        y + titlebar_height * 0.5,
        compact ? 10.0 : 11.0,
        {1.0, 1.0, 1.0, 0.42},
        false,
        "Inter",
        true
    );

    if (compact) {
        const double preview_y = y + 45.0;
        const double gap = 7.0;
        const double cell_width = (width - 26.0 - gap * 2.0) / 3.0;
        for (int index = 0; index < 3; ++index) {
            rounded_rectangle(
                cr,
                x + 13.0 + static_cast<double>(index) * (cell_width + gap),
                preview_y,
                cell_width,
                8.0,
                3.0
            );
            set_source(cr, style.accent, 0.12);
            cairo_fill(cr);
        }
        return;
    }

    const double preview_x = x + 18.0;
    const double preview_y = y + 52.0;
    const double preview_width = width - 36.0;
    const double preview_height = std::max(height - 68.0, 0.0);
    const double gap = 9.0;
    const double left_width = (preview_width - gap) / 2.4;
    const double right_width = preview_width - left_width - gap;
    const double row_height = std::max((preview_height - gap) * 0.5, 0.0);

    const std::array<std::array<double, 4>, 4> cells{{
        {preview_x, preview_y, left_width, row_height},
        {preview_x + left_width + gap, preview_y, right_width, preview_height},
        {preview_x, preview_y + row_height + gap, left_width, row_height},
        {preview_x, preview_y + preview_height - std::min(34.0, preview_height), preview_width, std::min(34.0, preview_height)},
    }};
    for (const auto& cell : cells) {
        rounded_rectangle(cr, cell[0], cell[1], cell[2], cell[3], 6.0);
        set_source(cr, style.accent, 0.10);
        cairo_fill_preserve(cr);
        set_source(cr, {1.0, 1.0, 1.0, 0.035});
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }
}

void draw_identity(
    cairo_t* cr,
    const RealmStyle& style,
    double center_y,
    bool active
) {
    const double scale = active ? 1.0 : 0.84;
    cairo_save(cr);
    cairo_translate(cr, 112.0, center_y);
    cairo_scale(cr, scale, scale);

    draw_text(
        cr,
        style.roman,
        0.0,
        0.0,
        60.0,
        style.accent_soft,
        false,
        "Georgia"
    );
    draw_text(
        cr,
        style.element,
        78.0,
        -14.0,
        23.0,
        style.accent_soft,
        false,
        "Georgia",
        false,
        3
    );
    draw_text(
        cr,
        style.place,
        78.0,
        18.0,
        13.0,
        {1.0, 1.0, 1.0, active ? 0.55 : 0.46},
        false,
        "Inter",
        false,
        2
    );
    cairo_restore(cr);
}

void draw_frontier(
    cairo_t* cr,
    const std::array<Point, 13>& points,
    double upward,
    double downward,
    const Color& top,
    const Color& bottom,
    double opacity
) {
    frontier_path(cr, points, upward, downward);
    double min_y = points.front().y;
    double max_y = points.front().y;
    for (const auto& point : points) {
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    cairo_pattern_t* gradient = cairo_pattern_create_linear(
        0.0,
        min_y - upward,
        0.0,
        max_y + downward
    );
    cairo_pattern_add_color_stop_rgba(
        gradient, 0.0, top.red, top.green, top.blue, top.alpha * opacity
    );
    cairo_pattern_add_color_stop_rgba(
        gradient, 1.0, bottom.red, bottom.green, bottom.blue, bottom.alpha * opacity
    );
    cairo_set_source(cr, gradient);
    cairo_fill(cr);
    cairo_pattern_destroy(gradient);
}

} // namespace

WorkspaceOverviewOverlay::WorkspaceOverviewOverlay(GtkApplication* app) {
    window_ = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(window_, "Realmheart Workspace Overview");
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(window_), "realmheart-workspace-overview-window");

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-workspace-overview";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::Exclusive;
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    apply_layer_surface(window_, spec);
    gtk_layer_set_exclusive_zone(window_, -1);

    canvas_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(canvas_, TRUE);
    gtk_widget_set_vexpand(canvas_, TRUE);
    gtk_widget_set_focusable(canvas_, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(canvas_),
        &WorkspaceOverviewOverlay::draw_callback,
        this,
        nullptr
    );

    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(
        keys,
        "key-pressed",
        G_CALLBACK(+[](
            GtkEventControllerKey*,
            guint keyval,
            guint,
            GdkModifierType,
            gpointer data
        ) -> gboolean {
            auto* self = static_cast<WorkspaceOverviewOverlay*>(data);
            if (keyval == GDK_KEY_Escape) {
                self->hide();
                return GDK_EVENT_STOP;
            }
            if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_4) {
                self->active_index_ = static_cast<int>(keyval - GDK_KEY_1);
                gtk_widget_queue_draw(self->canvas_);
                return GDK_EVENT_STOP;
            }
            return GDK_EVENT_PROPAGATE;
        }),
        this
    );
    gtk_widget_add_controller(GTK_WIDGET(window_), keys);

    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(
        click,
        "released",
        G_CALLBACK(+[](
            GtkGestureClick*,
            int,
            double x,
            double y,
            gpointer data
        ) {
            static_cast<WorkspaceOverviewOverlay*>(data)->activate_at(x, y);
        }),
        this
    );
    gtk_widget_add_controller(canvas_, GTK_EVENT_CONTROLLER(click));

    gtk_window_set_child(window_, canvas_);
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

WorkspaceOverviewOverlay::~WorkspaceOverviewOverlay() {
    release_assets();
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
        window_ = nullptr;
    }
    canvas_ = nullptr;
}

void WorkspaceOverviewOverlay::show() {
    static_cast<void>(ensure_assets());
    gtk_window_present(window_);
    gtk_widget_grab_focus(canvas_);
}

void WorkspaceOverviewOverlay::hide() {
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

void WorkspaceOverviewOverlay::toggle() {
    if (visible()) hide(); else show();
}

bool WorkspaceOverviewOverlay::visible() const {
    return window_ != nullptr && gtk_widget_get_visible(GTK_WIDGET(window_));
}

void WorkspaceOverviewOverlay::draw_callback(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer data
) {
    static_cast<WorkspaceOverviewOverlay*>(data)->draw(cr, width, height);
}

void WorkspaceOverviewOverlay::draw(cairo_t* cr, int width, int height) {
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    if (width <= 0 || height <= 0) return;

    const double scale = std::min(
        static_cast<double>(width) / kReferenceWidth,
        static_cast<double>(height) / kReferenceHeight
    );
    const double stage_width = kReferenceWidth * scale;
    const double stage_height = kReferenceHeight * scale;
    const double offset_x = (static_cast<double>(width) - stage_width) * 0.5;
    const double offset_y = (static_cast<double>(height) - stage_height) * 0.5;

    cairo_save(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);

    cairo_set_source_rgb(cr, 0.008, 0.012, 0.016);
    cairo_rectangle(cr, 0.0, 0.0, kReferenceWidth, kReferenceHeight);
    cairo_fill(cr);

    if (!ensure_assets()) {
        draw_text(
            cr,
            asset_error_.empty() ? "Workspace overview assets unavailable" : asset_error_,
            kReferenceWidth * 0.5,
            kReferenceHeight * 0.5,
            24.0,
            {0.95, 0.82, 0.55, 1.0},
            true,
            "Inter",
            true
        );
        cairo_restore(cr);
        return;
    }

    const auto heights = realm_heights(active_index_);
    const auto tops = realm_tops(heights);
    const auto boundary_one = boundary_points(tops[1], 0);
    const auto boundary_two = boundary_points(tops[2], 1);
    const auto boundary_three = boundary_points(tops[3], 2);
    const auto top_flat = flat_boundary(-30.0);
    const auto bottom_flat = flat_boundary(kReferenceHeight + 30.0);

    const std::array<std::array<Point, 13>, 4> clip_tops{{
        top_flat,
        shifted(boundary_one, -12.0),
        shifted(boundary_two, -10.0),
        shifted(boundary_three, -13.0),
    }};
    const std::array<std::array<Point, 13>, 4> clip_bottoms{{
        shifted(boundary_one, 18.0),
        shifted(boundary_two, 14.0),
        shifted(boundary_three, 18.0),
        bottom_flat,
    }};

    for (std::size_t index = 0; index < kRealms.size(); ++index) {
        const bool active = static_cast<int>(index) == active_index_;
        const auto& style = kRealms[index];
        const double top = tops[index];
        const double realm_height = heights[index];

        cairo_save(cr);
        polygon_path(cr, clip_tops[index], clip_bottoms[index]);
        cairo_clip(cr);

        draw_surface_cover(
            cr,
            assets_[index].background,
            0.0,
            top,
            kReferenceWidth,
            realm_height
        );

        if (!active) {
            set_source(cr, {0.0, 0.0, 0.0, 0.24});
            cairo_rectangle(cr, 0.0, top, kReferenceWidth, realm_height);
            cairo_fill(cr);
        }
        draw_atmosphere(cr, top, realm_height, active);

        const double character_height = active
            ? std::min(style.active_height, realm_height * 1.34)
            : style.idle_height;
        const double character_top = active
            ? top + std::max(8.0, realm_height * 0.02)
            : top + style.idle_top;
        const double character_right = active
            ? style.active_right
            : style.idle_right;
        draw_character(
            cr,
            assets_[index].character,
            character_height,
            character_right,
            character_top
        );

        draw_identity(
            cr,
            style,
            top + realm_height * 0.5,
            active
        );

        if (active) {
            const double windows_top = top + 68.0;
            const double windows_height = std::max(360.0, realm_height - 118.0);
            draw_window_card(
                cr, 355.0, windows_top + windows_height * 0.02,
                520.0, 255.0, style, kFakeWindows[index][0], false
            );
            draw_window_card(
                cr, 903.0, windows_top + windows_height * 0.12,
                350.0, 220.0, style, kFakeWindows[index][1], false
            );
            draw_window_card(
                cr, 640.0, windows_top + windows_height * 0.57,
                430.0, 160.0, style, kFakeWindows[index][2], false
            );

            draw_text(
                cr,
                style.character,
                kReferenceWidth - 52.0,
                top + realm_height - 85.0,
                21.0,
                style.accent_soft,
                false,
                "Georgia",
                true,
                1
            );
            draw_text(
                cr,
                style.place,
                kReferenceWidth - 52.0,
                top + realm_height - 57.0,
                11.0,
                {1.0, 1.0, 1.0, 0.55},
                false,
                "Inter",
                true,
                2
            );
        } else {
            const double windows_top =
                top + std::max(18.0, (realm_height - 72.0) * 0.5);
            draw_window_card(
                cr, 355.0, windows_top,
                320.0, 72.0, style, kFakeWindows[index][0], true
            );
            draw_window_card(
                cr, 697.0, windows_top,
                270.0, 72.0, style, kFakeWindows[index][1], true
            );
        }

        cairo_restore(cr);
    }

    draw_frontier(
        cr,
        boundary_one,
        6.0,
        12.0,
        {0.129, 0.075, 0.051, 1.0},
        {0.020, 0.024, 0.027, 0.15},
        0.68
    );
    draw_frontier(
        cr,
        boundary_two,
        3.0,
        9.0,
        {0.835, 0.945, 0.957, 0.24},
        {0.024, 0.078, 0.102, 0.03},
        0.42
    );
    draw_frontier(
        cr,
        boundary_three,
        7.0,
        13.0,
        {0.102, 0.090, 0.075, 1.0},
        {0.008, 0.012, 0.012, 0.08},
        0.70
    );

    cairo_pattern_t* horizontal_vignette =
        cairo_pattern_create_linear(0.0, 0.0, kReferenceWidth, 0.0);
    cairo_pattern_add_color_stop_rgba(horizontal_vignette, 0.0, 0.0, 0.0, 0.0, 0.18);
    cairo_pattern_add_color_stop_rgba(horizontal_vignette, 0.12, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(horizontal_vignette, 0.86, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(horizontal_vignette, 1.0, 0.0, 0.0, 0.0, 0.08);
    cairo_rectangle(cr, 0.0, 0.0, kReferenceWidth, kReferenceHeight);
    cairo_set_source(cr, horizontal_vignette);
    cairo_fill(cr);
    cairo_pattern_destroy(horizontal_vignette);

    cairo_pattern_t* vertical_vignette =
        cairo_pattern_create_linear(0.0, 0.0, 0.0, kReferenceHeight);
    cairo_pattern_add_color_stop_rgba(vertical_vignette, 0.0, 0.0, 0.0, 0.0, 0.12);
    cairo_pattern_add_color_stop_rgba(vertical_vignette, 0.09, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(vertical_vignette, 0.91, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(vertical_vignette, 1.0, 0.0, 0.0, 0.0, 0.25);
    cairo_rectangle(cr, 0.0, 0.0, kReferenceWidth, kReferenceHeight);
    cairo_set_source(cr, vertical_vignette);
    cairo_fill(cr);
    cairo_pattern_destroy(vertical_vignette);

    cairo_restore(cr);
}

void WorkspaceOverviewOverlay::activate_at(double x, double y) {
    if (canvas_ == nullptr) return;
    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    if (width <= 0 || height <= 0) return;

    const double scale = std::min(
        static_cast<double>(width) / kReferenceWidth,
        static_cast<double>(height) / kReferenceHeight
    );
    const double stage_width = kReferenceWidth * scale;
    const double stage_height = kReferenceHeight * scale;
    const double offset_x = (static_cast<double>(width) - stage_width) * 0.5;
    const double offset_y = (static_cast<double>(height) - stage_height) * 0.5;
    if (x < offset_x || x > offset_x + stage_width ||
        y < offset_y || y > offset_y + stage_height) {
        return;
    }

    const double reference_y = (y - offset_y) / scale;
    const auto heights = realm_heights(active_index_);
    double bottom = 0.0;
    for (int index = 0; index < 4; ++index) {
        bottom += heights[static_cast<std::size_t>(index)];
        if (reference_y < bottom) {
            active_index_ = index;
            gtk_widget_queue_draw(canvas_);
            return;
        }
    }
}

bool WorkspaceOverviewOverlay::ensure_assets() {
    if (assets_attempted_) return asset_error_.empty();
    assets_attempted_ = true;

    for (std::size_t index = 0; index < kRealms.size(); ++index) {
        const auto background = resolve_project_asset(kRealms[index].background_asset);
        const auto character = resolve_project_asset(kRealms[index].character_asset);
        if (!background || !character) {
            asset_error_ = "Unable to resolve workspace overview assets";
            std::cerr << "[WorkspaceOverview] " << asset_error_ << '\n';
            release_assets();
            return false;
        }

        assets_[index].background = load_cairo_surface(
            *background,
            asset_error_
        );
        if (assets_[index].background == nullptr) {
            release_assets();
            std::cerr << "[WorkspaceOverview] " << asset_error_ << '\n';
            return false;
        }

        assets_[index].character = load_cairo_surface(
            *character,
            asset_error_
        );
        if (assets_[index].character == nullptr) {
            release_assets();
            std::cerr << "[WorkspaceOverview] " << asset_error_ << '\n';
            return false;
        }
    }

    std::cerr << "[WorkspaceOverview] 1080p visual assets loaded lazily\n";
    return true;
}

void WorkspaceOverviewOverlay::release_assets() noexcept {
    for (auto& realm : assets_) {
        if (realm.background != nullptr) {
            cairo_surface_destroy(realm.background);
            realm.background = nullptr;
        }
        if (realm.character != nullptr) {
            cairo_surface_destroy(realm.character);
            realm.character = nullptr;
        }
    }
}

} // namespace realmheart::ui::workspace
