#include "ui/workspace/WorkspaceOverviewOverlay.hpp"
#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/BarGeometry.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace realmheart::ui::workspace {
namespace {

using SnapshotDrawFunc = void (*)(GtkWidget*, GtkSnapshot*, gpointer);

typedef struct _RealmheartWorkspaceOverviewCanvas {
    GtkWidget parent_instance;
    SnapshotDrawFunc draw_func;
    gpointer user_data;
} RealmheartWorkspaceOverviewCanvas;

typedef struct _RealmheartWorkspaceOverviewCanvasClass {
    GtkWidgetClass parent_class;
} RealmheartWorkspaceOverviewCanvasClass;

G_DEFINE_TYPE(
    RealmheartWorkspaceOverviewCanvas,
    realmheart_workspace_overview_canvas,
    GTK_TYPE_WIDGET
)

void realmheart_workspace_overview_canvas_snapshot(
    GtkWidget* widget,
    GtkSnapshot* snapshot
) {
    auto* self = reinterpret_cast<RealmheartWorkspaceOverviewCanvas*>(widget);
    if (self->draw_func != nullptr) {
        self->draw_func(widget, snapshot, self->user_data);
    }
}

void realmheart_workspace_overview_canvas_class_init(
    RealmheartWorkspaceOverviewCanvasClass* klass
) {
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = realmheart_workspace_overview_canvas_snapshot;
    gtk_widget_class_set_css_name(
        widget_class,
        "realmheart-workspace-overview-canvas"
    );
}

void realmheart_workspace_overview_canvas_init(
    RealmheartWorkspaceOverviewCanvas* self
) {
    self->draw_func = nullptr;
    self->user_data = nullptr;
}

GtkWidget* overview_canvas_new(
    SnapshotDrawFunc draw_func,
    gpointer user_data
) {
    auto* self = reinterpret_cast<RealmheartWorkspaceOverviewCanvas*>(
        g_object_new(
            realmheart_workspace_overview_canvas_get_type(),
            nullptr
        )
    );
    self->draw_func = draw_func;
    self->user_data = user_data;
    return GTK_WIDGET(self);
}

void overview_canvas_clear(GtkWidget* widget) {
    if (widget == nullptr) return;
    auto* self = reinterpret_cast<RealmheartWorkspaceOverviewCanvas*>(widget);
    self->draw_func = nullptr;
    self->user_data = nullptr;
}

constexpr double kReferenceWidth = 1920.0;
constexpr double kReferenceHeight = 1080.0;
constexpr double kActiveFraction = 0.56;
constexpr double kInactiveFraction = (1.0 - kActiveFraction) / 3.0;
constexpr double kTransitionDurationSeconds = 0.480;
constexpr double kCardTransitionDurationSeconds = 0.180;
constexpr double kCardTransitionDistance = 18.0;
constexpr double kCardDragThresholdPixels = 8.0;
constexpr double kDraggedCardScale = 1.025;
constexpr double kInactiveBackgroundZoom = 1.62;
constexpr double kActiveBackgroundZoom = 1.10;
constexpr gint64 kMicrosecondsPerSecond = 1'000'000;

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
constexpr GdkMemoryFormat kCairoArgb32MemoryFormat =
    GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
constexpr GdkMemoryFormat kCairoArgb32MemoryFormat =
    GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] bool contains(double point_x, double point_y) const noexcept {
        return point_x >= x && point_x <= x + width &&
            point_y >= y && point_y <= y + height;
    }
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

constexpr std::size_t kBoundarySampleCount = 65;
using BoundaryPoints = std::array<Point, kBoundarySampleCount>;

struct RippleSpec {
    double primary_amplitude = 0.0;
    double primary_cycles = 1.0;
    double secondary_amplitude = 0.0;
    double secondary_cycles = 2.0;
    double phase = 0.0;
};

// Smooth, restrained waves. The phases differ so the three boundaries do not
// read as duplicated sine curves, but none of them has the jagged fault-line
// silhouette of the previous shader.
constexpr std::array<RippleSpec, 3> kRippleSpecs{{
    {6.4, 1.10, 1.8, 2.55, 0.20},
    {7.2, 0.92, 2.0, 2.20, 1.05},
    {5.8, 1.24, 1.6, 2.85, 2.10},
}};


double lerp(double from, double to, double progress) {
    return from + (to - from) * progress;
}

Rect lerp_rect(const Rect& from, const Rect& to, double progress) {
    return {
        lerp(from.x, to.x, progress),
        lerp(from.y, to.y, progress),
        lerp(from.width, to.width, progress),
        lerp(from.height, to.height, progress),
    };
}

double cubic_bezier_coordinate(double t, double first, double second) {
    const double inverse = 1.0 - t;
    return 3.0 * inverse * inverse * t * first +
        3.0 * inverse * t * t * second + t * t * t;
}

double cubic_bezier_derivative(double t, double first, double second) {
    const double inverse = 1.0 - t;
    return 3.0 * inverse * inverse * first +
        6.0 * inverse * t * (second - first) +
        3.0 * t * t * (1.0 - second);
}

// CSS cubic-bezier(.22, .78, .2, 1). Solve X first, then sample Y.
double transition_ease(double progress) {
    const double x = std::clamp(progress, 0.0, 1.0);
    double t = x;
    for (int iteration = 0; iteration < 6; ++iteration) {
        const double error = cubic_bezier_coordinate(t, 0.22, 0.20) - x;
        const double derivative = cubic_bezier_derivative(t, 0.22, 0.20);
        if (std::abs(derivative) < 0.000001) break;
        t = std::clamp(t - error / derivative, 0.0, 1.0);
    }
    return cubic_bezier_coordinate(t, 0.78, 1.0);
}

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

std::array<double, 4> target_realm_heights(int active_index) {
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

double realm_activity(double height) {
    const double inactive_height = kInactiveFraction * kReferenceHeight;
    const double active_height = kActiveFraction * kReferenceHeight;
    return std::clamp(
        (height - inactive_height) / (active_height - inactive_height),
        0.0,
        1.0
    );
}

BoundaryPoints boundary_points(double base, std::size_t boundary_index) {
    BoundaryPoints points{};
    const auto& ripple = kRippleSpecs[boundary_index];
    constexpr double kTau = 6.28318530717958647692;

    for (std::size_t index = 0; index < points.size(); ++index) {
        const double progress = static_cast<double>(index) /
            static_cast<double>(points.size() - 1U);
        const double primary = std::sin(
            progress * kTau * ripple.primary_cycles + ripple.phase
        ) * ripple.primary_amplitude;
        const double secondary = std::sin(
            progress * kTau * ripple.secondary_cycles - ripple.phase * 0.65
        ) * ripple.secondary_amplitude;
        points[index] = {
            progress * kReferenceWidth,
            base + primary + secondary,
        };
    }
    return points;
}

BoundaryPoints shifted(const BoundaryPoints& points, double delta_y) {
    auto result = points;
    for (auto& point : result) point.y += delta_y;
    return result;
}

struct CardVisual {
    Rect rect;
    double detail = 0.0;
    double opacity = 1.0;
};

std::array<CardVisual, 3> window_card_visuals(
    double top,
    double realm_height,
    double activity
) {
    const double compact_top =
        top + std::max(18.0, (realm_height - 72.0) * 0.5);
    const std::array<Rect, 3> compact{{
        {355.0, compact_top, 320.0, 72.0},
        {697.0, compact_top, 270.0, 72.0},
        {790.5, compact_top + 10.0, 301.0, 52.0},
    }};

    const double windows_top = top + 68.0;
    const double windows_height = std::max(360.0, realm_height - 118.0);
    const std::array<Rect, 3> expanded{{
        {355.0, windows_top + windows_height * 0.02, 520.0, 255.0},
        {903.0, windows_top + windows_height * 0.12, 350.0, 220.0},
        {640.0, windows_top + windows_height * 0.57, 430.0, 160.0},
    }};

    std::array<CardVisual, 3> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = {
            lerp_rect(compact[index], expanded[index], activity),
            activity,
            index == 2U ? activity : 1.0,
        };
    }
    return result;
}

Rect scaled_card_rect(
    std::size_t slot,
    bool expanded,
    double top,
    double realm_height
) {
    const double native_height = (expanded ? kActiveFraction : kInactiveFraction) *
        kReferenceHeight;
    const auto native_cards = window_card_visuals(
        0.0,
        native_height,
        expanded ? 1.0 : 0.0
    );
    if (slot >= native_cards.size() || native_height <= 0.0) return {};

    const auto& native = native_cards[slot].rect;
    const double scale_y = realm_height / native_height;
    return {
        native.x,
        top + native.y * scale_y,
        native.width,
        native.height * scale_y,
    };
}

bool same_card_identity(
    const WorkspaceOverviewCard& left,
    const WorkspaceOverviewCard& right
) noexcept {
    if (left.summary || right.summary) {
        return left.summary && right.summary;
    }
    return !left.address.empty() && left.address == right.address;
}

std::optional<std::size_t> find_card_slot(
    const WorkspaceOverviewRealm& realm,
    const WorkspaceOverviewCard& card
) {
    for (std::size_t slot = 0; slot < realm.card_count; ++slot) {
        if (same_card_identity(realm.cards[slot], card)) return slot;
    }
    return std::nullopt;
}

Rect identity_rect(double top, double realm_height, double activity) {
    (void)activity;
    constexpr double width = 254.0;
    constexpr double height = 70.0;
    return {
        96.0,
        top + realm_height * 0.5 - height * 0.5,
        width,
        height,
    };
}

std::optional<int> hit_realm_identity(
    double x,
    double y,
    const std::array<double, 4>& heights
) {
    const auto tops = realm_tops(heights);
    for (std::size_t index = 0; index < kRealms.size(); ++index) {
        const double activity = realm_activity(heights[index]);
        if (identity_rect(tops[index], heights[index], activity).contains(x, y)) {
            return static_cast<int>(index);
        }
    }
    return std::nullopt;
}

struct WorkspaceCardHit {
    std::size_t realm_index = 0;
    std::size_t card_index = 0;
};

std::optional<WorkspaceCardHit> hit_realm_card(
    double x,
    double y,
    const std::array<double, 4>& heights,
    const WorkspaceOverviewState& workspace_state
) {
    const auto tops = realm_tops(heights);
    for (std::size_t index = 0; index < kRealms.size(); ++index) {
        const double activity = realm_activity(heights[index]);
        const auto cards = window_card_visuals(tops[index], heights[index], activity);
        const auto count = std::min(
            workspace_state[index].card_count,
            cards.size()
        );
        for (std::size_t card_index = 0; card_index < count; ++card_index) {
            const auto& card = cards[card_index];
            if (card.opacity > 0.05 && card.rect.contains(x, y)) {
                return WorkspaceCardHit{index, card_index};
            }
        }
    }
    return std::nullopt;
}

std::optional<int> realm_index_at_point(
    double x,
    double y,
    const std::array<double, 4>& heights
) {
    if (x < 0.0 || x > kReferenceWidth || y < 0.0 || y > kReferenceHeight) {
        return std::nullopt;
    }

    double bottom = 0.0;
    for (std::size_t index = 0; index < heights.size(); ++index) {
        bottom += heights[index];
        if (y <= bottom) return static_cast<int>(index);
    }
    return std::nullopt;
}

[[nodiscard]] GdkTexture* load_texture(
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
    }
    return texture;
}

void draw_window_card(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height,
    const RealmStyle& style,
    std::pair<std::string_view, std::string_view> content,
    double detail,
    double opacity
) {
    if (opacity <= 0.001 || width <= 0.0 || height <= 0.0) return;

    cairo_save(cr);
    cairo_push_group(cr);

    const double radius = lerp(11.0, 15.0, detail);
    rounded_rectangle(cr, x, y, width, height, radius);
    cairo_pattern_t* background = cairo_pattern_create_linear(x, y, x + width, y + height);
    cairo_pattern_add_color_stop_rgba(background, 0.0, 0.035, 0.047, 0.059, 0.94);
    cairo_pattern_add_color_stop_rgba(background, 1.0, 0.051, 0.067, 0.082, 0.84);
    cairo_set_source(cr, background);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(background);

    set_source(cr, style.accent, 0.34);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    const double titlebar_height = lerp(34.0, 38.0, detail);
    set_source(cr, {0.0, 0.0, 0.0, 0.17});
    cairo_rectangle(cr, x, y, width, titlebar_height);
    cairo_fill(cr);

    set_source(cr, style.accent);
    cairo_arc(
        cr,
        x + 18.0,
        y + titlebar_height * 0.5,
        lerp(4.2, 5.0, detail),
        0.0,
        2.0 * std::acos(-1.0)
    );
    cairo_fill(cr);

    draw_text(
        cr,
        content.first,
        x + 33.0,
        y + titlebar_height * 0.5,
        lerp(12.0, 14.0, detail),
        {0.95, 0.94, 0.91, 0.96},
        true
    );
    draw_text(
        cr,
        content.second,
        x + width - 14.0,
        y + titlebar_height * 0.5,
        lerp(10.0, 11.0, detail),
        {1.0, 1.0, 1.0, 0.42},
        false,
        "Inter",
        true
    );

    const double compact_opacity = 1.0 - detail;
    if (compact_opacity > 0.001) {
        const double preview_y = y + lerp(45.0, 52.0, detail);
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
            set_source(cr, style.accent, 0.12 * compact_opacity);
            cairo_fill(cr);
        }
    }

    if (detail > 0.001) {
        const double preview_x = x + 18.0;
        const double preview_y = y + 52.0;
        const double preview_width = std::max(width - 36.0, 0.0);
        const double preview_height = std::max(height - 68.0, 0.0);
        const double gap = 9.0;
        const double left_width = std::max((preview_width - gap) / 2.4, 0.0);
        const double right_width = std::max(preview_width - left_width - gap, 0.0);
        const double row_height = std::max((preview_height - gap) * 0.5, 0.0);

        const std::array<std::array<double, 4>, 4> cells{{
            {preview_x, preview_y, left_width, row_height},
            {preview_x + left_width + gap, preview_y, right_width, preview_height},
            {preview_x, preview_y + row_height + gap, left_width, row_height},
            {
                preview_x,
                preview_y + preview_height - std::min(34.0, preview_height),
                preview_width,
                std::min(34.0, preview_height),
            },
        }};
        for (const auto& cell : cells) {
            if (cell[2] <= 0.0 || cell[3] <= 0.0) continue;
            rounded_rectangle(cr, cell[0], cell[1], cell[2], cell[3], 6.0);
            set_source(cr, style.accent, 0.10 * detail);
            cairo_fill_preserve(cr);
            set_source(cr, {1.0, 1.0, 1.0, 0.035 * detail});
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
        }
    }

    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, std::clamp(opacity, 0.0, 1.0));
    cairo_restore(cr);
}

[[nodiscard]] PangoLayout* create_identity_layout(
    GtkWidget* widget,
    std::string_view text,
    double size,
    std::string_view family,
    int letter_spacing,
    std::string& error_message
) {
    const std::string owned(text);
    PangoLayout* layout = gtk_widget_create_pango_layout(widget, owned.c_str());
    if (layout == nullptr) {
        error_message = "Unable to create workspace identity layout";
        return nullptr;
    }

    PangoFontDescription* font = pango_font_description_new();
    const std::string family_name(family);
    pango_font_description_set_family(font, family_name.c_str());
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);

    if (letter_spacing != 0) {
        PangoAttrList* attributes = pango_attr_list_new();
        pango_attr_list_insert(
            attributes,
            pango_attr_letter_spacing_new(letter_spacing * PANGO_SCALE)
        );
        pango_layout_set_attributes(layout, attributes);
        pango_attr_list_unref(attributes);
    }
    return layout;
}

void append_identity_layout(
    GtkSnapshot* snapshot,
    PangoLayout* layout,
    double x,
    double center_y,
    const Color& color,
    const Color* shadow_color = nullptr,
    double shadow_strength = 0.0
) {
    if (layout == nullptr) return;

    int text_height = 0;
    pango_layout_get_pixel_size(layout, nullptr, &text_height);
    const graphene_point_t offset = GRAPHENE_POINT_INIT(
        static_cast<float>(x),
        static_cast<float>(center_y - static_cast<double>(text_height) * 0.5)
    );
    const GdkRGBA rgba{
        static_cast<float>(color.red),
        static_cast<float>(color.green),
        static_cast<float>(color.blue),
        static_cast<float>(color.alpha),
    };

    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &offset);
    if (shadow_color != nullptr && shadow_strength > 0.0) {
        const GdkRGBA shadow_rgba{
            static_cast<float>(shadow_color->red),
            static_cast<float>(shadow_color->green),
            static_cast<float>(shadow_color->blue),
            static_cast<float>(shadow_color->alpha * shadow_strength),
        };
        const graphene_point_t shadow_offset_far = GRAPHENE_POINT_INIT(0.0F, 3.0F);
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &shadow_offset_far);
        gtk_snapshot_append_layout(snapshot, layout, &shadow_rgba);
        gtk_snapshot_restore(snapshot);

        const GdkRGBA shadow_rgba_near{
            static_cast<float>(shadow_color->red),
            static_cast<float>(shadow_color->green),
            static_cast<float>(shadow_color->blue),
            static_cast<float>(shadow_color->alpha * shadow_strength * 1.35),
        };
        const graphene_point_t shadow_offset_near = GRAPHENE_POINT_INIT(0.0F, 1.5F);
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &shadow_offset_near);
        gtk_snapshot_append_layout(snapshot, layout, &shadow_rgba_near);
        gtk_snapshot_restore(snapshot);
    }
    gtk_snapshot_append_layout(snapshot, layout, &rgba);
    gtk_snapshot_restore(snapshot);
}

[[nodiscard]] GdkTexture* texture_from_surface(
    cairo_surface_t* surface,
    std::string& error_message
) {
    if (surface == nullptr ||
        cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
        cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32) {
        error_message = "Unable to convert workspace scene into a GDK texture";
        return nullptr;
    }

    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const auto* pixels = cairo_image_surface_get_data(surface);
    if (width <= 0 || height <= 0 || stride < width * 4 || pixels == nullptr) {
        error_message = "Workspace scene produced invalid texture data";
        return nullptr;
    }

    const gsize byte_count = static_cast<gsize>(stride) *
        static_cast<gsize>(height);
    GBytes* bytes = g_bytes_new(pixels, byte_count);
    GdkTexture* texture = gdk_memory_texture_new(
        width,
        height,
        kCairoArgb32MemoryFormat,
        bytes,
        static_cast<gsize>(stride)
    );
    g_bytes_unref(bytes);
    if (texture == nullptr) {
        error_message = "Unable to allocate workspace GDK texture";
    }
    return texture;
}

[[nodiscard]] GdkTexture* render_card_texture(
    std::size_t realm_index,
    const WorkspaceOverviewCard& content,
    std::size_t slot,
    bool expanded,
    std::string& error_message
) {
    const double native_height = (expanded ? kActiveFraction : kInactiveFraction) *
        kReferenceHeight;
    const auto cards = window_card_visuals(
        0.0,
        native_height,
        expanded ? 1.0 : 0.0
    );
    if (realm_index >= kRealms.size() || slot >= cards.size()) {
        error_message = "Invalid workspace card render request";
        return nullptr;
    }

    const auto& visual = cards[slot];
    const int pixel_width = std::max(
        1,
        static_cast<int>(std::ceil(visual.rect.width))
    );
    const int pixel_height = std::max(
        1,
        static_cast<int>(std::ceil(visual.rect.height))
    );
    cairo_surface_t* surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32,
        pixel_width,
        pixel_height
    );
    const cairo_status_t surface_status = cairo_surface_status(surface);
    if (surface_status != CAIRO_STATUS_SUCCESS) {
        error_message = "Unable to allocate workspace card surface: ";
        error_message += cairo_status_to_string(surface_status);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    cairo_t* cr = cairo_create(surface);
    const cairo_status_t context_status = cairo_status(cr);
    if (context_status != CAIRO_STATUS_SUCCESS) {
        error_message = "Unable to create workspace card renderer: ";
        error_message += cairo_status_to_string(context_status);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    draw_window_card(
        cr,
        0.0,
        0.0,
        visual.rect.width,
        visual.rect.height,
        kRealms[realm_index],
        {content.app_name, content.title},
        visual.detail,
        visual.opacity
    );

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    GdkTexture* texture = texture_from_surface(surface, error_message);
    cairo_surface_destroy(surface);
    return texture;
}

[[nodiscard]] GdkTexture* render_realm_overlay_texture(
    std::size_t realm_index,
    const WorkspaceOverviewRealm&,
    bool expanded,
    std::string& error_message
) {
    const auto& style = kRealms[realm_index];
    const double realm_height = (expanded ? kActiveFraction : kInactiveFraction) *
        kReferenceHeight;
    const int pixel_width = static_cast<int>(kReferenceWidth);
    const int pixel_height = std::max(
        1,
        static_cast<int>(std::ceil(realm_height))
    );

    cairo_surface_t* surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32,
        pixel_width,
        pixel_height
    );
    const cairo_status_t surface_status = cairo_surface_status(surface);
    if (surface_status != CAIRO_STATUS_SUCCESS) {
        error_message = "Unable to allocate workspace overlay surface: ";
        error_message += cairo_status_to_string(surface_status);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    cairo_t* cr = cairo_create(surface);
    const cairo_status_t context_status = cairo_status(cr);
    if (context_status != CAIRO_STATUS_SUCCESS) {
        error_message = "Unable to create workspace overlay renderer: ";
        error_message += cairo_status_to_string(context_status);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (expanded) {
        draw_text(
            cr,
            style.character,
            kReferenceWidth - 52.0,
            realm_height - 85.0,
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
            realm_height - 57.0,
            11.0,
            {1.0, 1.0, 1.0, 0.55},
            false,
            "Inter",
            true,
            2
        );
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    GdkTexture* texture = texture_from_surface(surface, error_message);
    cairo_surface_destroy(surface);
    return texture;
}

[[nodiscard]] GskPath* polygon_gsk_path(
    const BoundaryPoints& top,
    const BoundaryPoints& bottom
) {
    GskPathBuilder* builder = gsk_path_builder_new();
    gsk_path_builder_move_to(
        builder,
        static_cast<float>(top.front().x),
        static_cast<float>(top.front().y)
    );
    for (std::size_t index = 1; index < top.size(); ++index) {
        gsk_path_builder_line_to(
            builder,
            static_cast<float>(top[index].x),
            static_cast<float>(top[index].y)
        );
    }
    for (auto index = bottom.size(); index-- > 0;) {
        gsk_path_builder_line_to(
            builder,
            static_cast<float>(bottom[index].x),
            static_cast<float>(bottom[index].y)
        );
    }
    gsk_path_builder_close(builder);
    return gsk_path_builder_free_to_path(builder);
}

[[nodiscard]] GskPath* create_separator_band_path(
    std::size_t boundary_index,
    double top_offset,
    double bottom_offset
) {
    const auto centerline = boundary_points(0.0, boundary_index);
    return polygon_gsk_path(
        shifted(centerline, top_offset),
        shifted(centerline, bottom_offset)
    );
}

[[nodiscard]] GskRenderNode* create_separator_node(
    std::size_t boundary_index
) {
    constexpr std::array<std::pair<double, double>, 4> kLayerOffsets{{
        {-9.5, 9.5},
        {-7.4, 7.4},
        {-7.4, -5.4},
        {5.4, 7.4},
    }};
    constexpr std::array<GdkRGBA, 3> kBodyColors{{
        {0.14F, 0.085F, 0.050F, 0.72F},
        {0.035F, 0.095F, 0.115F, 0.72F},
        {0.075F, 0.095F, 0.055F, 0.72F},
    }};
    constexpr std::array<GdkRGBA, 3> kUpperAccentColors{{
        {0.86F, 0.53F, 0.27F, 0.58F},
        {0.60F, 0.85F, 0.90F, 0.56F},
        {0.62F, 0.72F, 0.50F, 0.54F},
    }};
    constexpr std::array<GdkRGBA, 3> kLowerAccentColors{{
        {0.55F, 0.30F, 0.16F, 0.22F},
        {0.30F, 0.58F, 0.66F, 0.22F},
        {0.36F, 0.46F, 0.28F, 0.20F},
    }};
    const std::array<GdkRGBA, 4> colors{{
        {0.0F, 0.0F, 0.0F, 0.07F},
        kBodyColors[boundary_index],
        kUpperAccentColors[boundary_index],
        kLowerAccentColors[boundary_index],
    }};

    GtkSnapshot* separator_snapshot = gtk_snapshot_new();
    for (std::size_t layer = 0; layer < kLayerOffsets.size(); ++layer) {
        GskPath* path = create_separator_band_path(
            boundary_index,
            kLayerOffsets[layer].first,
            kLayerOffsets[layer].second
        );
        if (path == nullptr) continue;
        gtk_snapshot_append_fill(
            separator_snapshot,
            path,
            GSK_FILL_RULE_WINDING,
            &colors[layer]
        );
        gsk_path_unref(path);
    }
    return gtk_snapshot_free_to_node(separator_snapshot);
}

void append_ripple_separator(
    GtkSnapshot* snapshot,
    GskRenderNode* node,
    double y
) {
    if (node == nullptr) return;

    const graphene_point_t offset = GRAPHENE_POINT_INIT(
        0.0F,
        static_cast<float>(y)
    );
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &offset);
    gtk_snapshot_append_node(snapshot, node);
    gtk_snapshot_restore(snapshot);
}

void append_texture_with_opacity(
    GtkSnapshot* snapshot,
    GdkTexture* texture,
    const Rect& bounds,
    double opacity
) {
    if (texture == nullptr || opacity <= 0.001 ||
        bounds.width <= 0.0 || bounds.height <= 0.0) {
        return;
    }

    const bool translucent = opacity < 0.999;
    if (translucent) {
        gtk_snapshot_push_opacity(
            snapshot,
            static_cast<float>(std::clamp(opacity, 0.0, 1.0))
        );
    }
    const graphene_rect_t texture_bounds = GRAPHENE_RECT_INIT(
        static_cast<float>(bounds.x),
        static_cast<float>(bounds.y),
        static_cast<float>(bounds.width),
        static_cast<float>(bounds.height)
    );
    gtk_snapshot_append_scaled_texture(
        snapshot,
        texture,
        GSK_SCALING_FILTER_LINEAR,
        &texture_bounds
    );
    if (translucent) gtk_snapshot_pop(snapshot);
}

Rect translated_rect(Rect rect, double delta_x, double delta_y) {
    rect.x += delta_x;
    rect.y += delta_y;
    return rect;
}

Rect translated_rect(Rect rect, double delta_y) {
    return translated_rect(rect, 0.0, delta_y);
}

Rect scaled_around_center(Rect rect, double scale) {
    const double scaled_width = rect.width * scale;
    const double scaled_height = rect.height * scale;
    rect.x -= (scaled_width - rect.width) * 0.5;
    rect.y -= (scaled_height - rect.height) * 0.5;
    rect.width = scaled_width;
    rect.height = scaled_height;
    return rect;
}

void append_card_texture_pair(
    GtkSnapshot* snapshot,
    GdkTexture* compact,
    GdkTexture* expanded,
    const Rect& compact_bounds,
    const Rect& expanded_bounds,
    double activity,
    double opacity
) {
    append_texture_with_opacity(
        snapshot,
        compact,
        compact_bounds,
        opacity * (1.0 - activity)
    );
    append_texture_with_opacity(
        snapshot,
        expanded,
        expanded_bounds,
        opacity * activity
    );
}

Rect background_bounds(
    GdkTexture* texture,
    double top,
    double realm_height,
    double activity
) {
    if (texture == nullptr) return {};
    const int source_width = gdk_texture_get_width(texture);
    const int source_height = gdk_texture_get_height(texture);
    if (source_width <= 0 || source_height <= 0) return {};

    const double cover_scale = std::max(
        kReferenceWidth / static_cast<double>(source_width),
        realm_height / static_cast<double>(source_height)
    );
    const double zoom = lerp(
        kInactiveBackgroundZoom,
        kActiveBackgroundZoom,
        activity
    );
    const double width = static_cast<double>(source_width) * cover_scale * zoom;
    const double height = static_cast<double>(source_height) * cover_scale * zoom;

    return {
        kReferenceWidth - width,
        top + (realm_height - height) * 0.5,
        width,
        height,
    };
}

Rect character_bounds(
    GdkTexture* texture,
    const RealmStyle& style,
    double top,
    double realm_height,
    double activity
) {
    if (texture == nullptr) return {};
    const int source_width = gdk_texture_get_width(texture);
    const int source_height = gdk_texture_get_height(texture);
    if (source_width <= 0 || source_height <= 0) return {};

    const double active_top = std::max(8.0, realm_height * 0.02);
    const double target_height = lerp(
        style.idle_height,
        style.active_height,
        activity
    );
    const double target_width = static_cast<double>(source_width) *
        target_height / static_cast<double>(source_height);
    const double right = lerp(style.idle_right, style.active_right, activity);
    const double local_top = lerp(style.idle_top, active_top, activity);

    return {
        kReferenceWidth - right - target_width,
        top + local_top,
        target_width,
        target_height,
    };
}

void append_realm_atmosphere(
    GtkSnapshot* snapshot,
    double top,
    double height,
    double activity
) {
    if (height <= 0.0) return;

    const graphene_rect_t bounds = GRAPHENE_RECT_INIT(
        0.0F,
        static_cast<float>(top),
        static_cast<float>(kReferenceWidth),
        static_cast<float>(height)
    );

    const double idle_darkness = 0.24 * (1.0 - activity);
    if (idle_darkness > 0.001) {
        const GdkRGBA shade{
            0.0F,
            0.0F,
            0.0F,
            static_cast<float>(idle_darkness),
        };
        gtk_snapshot_append_color(snapshot, &shade, &bounds);
    }

    const graphene_point_t horizontal_start = GRAPHENE_POINT_INIT(
        0.0F,
        static_cast<float>(top)
    );
    const graphene_point_t horizontal_end = GRAPHENE_POINT_INIT(
        static_cast<float>(kReferenceWidth),
        static_cast<float>(top)
    );
    const std::array<GskColorStop, 5> horizontal_stops{{
        {0.0F, GdkRGBA{0.01F, 0.02F, 0.03F, 0.92F}},
        {0.15F, GdkRGBA{0.02F, 0.03F, 0.04F, static_cast<float>(lerp(0.72, 0.64, activity))}},
        {0.45F, GdkRGBA{0.02F, 0.03F, 0.04F, static_cast<float>(lerp(0.26, 0.17, activity))}},
        {0.75F, GdkRGBA{0.01F, 0.02F, 0.03F, static_cast<float>(lerp(0.04, 0.01, activity))}},
        {1.0F, GdkRGBA{0.0F, 0.0F, 0.0F, static_cast<float>(lerp(0.10, 0.08, activity))}},
    }};
    gtk_snapshot_append_linear_gradient(
        snapshot,
        &bounds,
        &horizontal_start,
        &horizontal_end,
        horizontal_stops.data(),
        horizontal_stops.size()
    );

    const graphene_point_t vertical_start = GRAPHENE_POINT_INIT(
        0.0F,
        static_cast<float>(top)
    );
    const graphene_point_t vertical_end = GRAPHENE_POINT_INIT(
        0.0F,
        static_cast<float>(top + height)
    );
    const std::array<GskColorStop, 3> vertical_stops{{
        {0.0F, GdkRGBA{0.0F, 0.0F, 0.0F, static_cast<float>(lerp(0.18, 0.08, activity))}},
        {0.45F, GdkRGBA{0.0F, 0.0F, 0.0F, 0.0F}},
        {1.0F, GdkRGBA{0.0F, 0.0F, 0.0F, static_cast<float>(lerp(0.33, 0.24, activity))}},
    }};
    gtk_snapshot_append_linear_gradient(
        snapshot,
        &bounds,
        &vertical_start,
        &vertical_end,
        vertical_stops.data(),
        vertical_stops.size()
    );
}

void append_global_vignette(GtkSnapshot* snapshot) {
    const graphene_rect_t bounds = GRAPHENE_RECT_INIT(
        0.0F,
        0.0F,
        static_cast<float>(kReferenceWidth),
        static_cast<float>(kReferenceHeight)
    );

    const graphene_point_t horizontal_start = GRAPHENE_POINT_INIT(0.0F, 0.0F);
    const graphene_point_t horizontal_end = GRAPHENE_POINT_INIT(
        static_cast<float>(kReferenceWidth),
        0.0F
    );
    const std::array<GskColorStop, 4> horizontal_stops{{
        {0.0F, GdkRGBA{0.0, 0.0, 0.0, 0.18}},
        {0.12F, GdkRGBA{0.0, 0.0, 0.0, 0.0}},
        {0.86F, GdkRGBA{0.0, 0.0, 0.0, 0.0}},
        {1.0F, GdkRGBA{0.0, 0.0, 0.0, 0.08}},
    }};
    gtk_snapshot_append_linear_gradient(
        snapshot,
        &bounds,
        &horizontal_start,
        &horizontal_end,
        horizontal_stops.data(),
        horizontal_stops.size()
    );

    const graphene_point_t vertical_start = GRAPHENE_POINT_INIT(0.0F, 0.0F);
    const graphene_point_t vertical_end = GRAPHENE_POINT_INIT(
        0.0F,
        static_cast<float>(kReferenceHeight)
    );
    const std::array<GskColorStop, 4> vertical_stops{{
        {0.0F, GdkRGBA{0.0, 0.0, 0.0, 0.12}},
        {0.09F, GdkRGBA{0.0, 0.0, 0.0, 0.0}},
        {0.91F, GdkRGBA{0.0, 0.0, 0.0, 0.0}},
        {1.0F, GdkRGBA{0.0, 0.0, 0.0, 0.25}},
    }};
    gtk_snapshot_append_linear_gradient(
        snapshot,
        &bounds,
        &vertical_start,
        &vertical_end,
        vertical_stops.data(),
        vertical_stops.size()
    );
}

} // namespace

WorkspaceOverviewOverlay::WorkspaceOverviewOverlay(
    GtkApplication* app,
    std::function<void(int)> activate_workspace,
    std::function<void(int, std::string)> activate_window,
    std::function<void(int, std::string)> move_window
) : activate_workspace_(std::move(activate_workspace)),
    activate_window_(std::move(activate_window)),
    move_window_(std::move(move_window)) {
    workspace_state_ = build_workspace_overview_state({});
    initialize_separator_nodes();
    displayed_heights_ = target_realm_heights(active_index_);
    animation_start_heights_ = displayed_heights_;
    animation_target_heights_ = displayed_heights_;
    for (auto& realm_slots : card_from_slots_) {
        for (std::size_t slot = 0; slot < realm_slots.size(); ++slot) {
            realm_slots[slot] = static_cast<int>(slot);
        }
    }

    window_ = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(window_, "Realmheart Workspace Overview");
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(window_), "realmheart-workspace-overview-window");

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-workspace-overview";
    spec.layer = LayerSurfaceLevel::Top;
    spec.keyboard_mode = LayerKeyboardMode::OnDemand;
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    spec.margin_left = 0;
    apply_layer_surface(window_, spec);

    canvas_ = overview_canvas_new(
        &WorkspaceOverviewOverlay::snapshot_callback,
        this
    );
    gtk_widget_set_hexpand(canvas_, TRUE);
    gtk_widget_set_vexpand(canvas_, TRUE);
    gtk_widget_set_focusable(canvas_, TRUE);

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
                self->select_realm(static_cast<int>(keyval - GDK_KEY_1));
                return GDK_EVENT_STOP;
            }
            return GDK_EVENT_PROPAGATE;
        }),
        this
    );
    gtk_widget_add_controller(GTK_WIDGET(window_), keys);

    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(
        drag,
        "drag-begin",
        G_CALLBACK(+[](
            GtkGestureDrag*,
            double x,
            double y,
            gpointer data
        ) {
            static_cast<WorkspaceOverviewOverlay*>(data)->handle_drag_begin(x, y);
        }),
        this
    );
    g_signal_connect(
        drag,
        "drag-update",
        G_CALLBACK(+[](
            GtkGestureDrag*,
            double offset_x,
            double offset_y,
            gpointer data
        ) {
            static_cast<WorkspaceOverviewOverlay*>(data)->handle_drag_update(
                offset_x,
                offset_y
            );
        }),
        this
    );
    g_signal_connect(
        drag,
        "drag-end",
        G_CALLBACK(+[](
            GtkGestureDrag*,
            double offset_x,
            double offset_y,
            gpointer data
        ) {
            static_cast<WorkspaceOverviewOverlay*>(data)->handle_drag_end(
                offset_x,
                offset_y
            );
        }),
        this
    );
    g_signal_connect(
        drag,
        "cancel",
        G_CALLBACK(+[](
            GtkGesture*,
            GdkEventSequence*,
            gpointer data
        ) {
            auto* self = static_cast<WorkspaceOverviewOverlay*>(data);
            self->reset_drag();
            if (self->canvas_ != nullptr) gtk_widget_queue_draw(self->canvas_);
        }),
        this
    );
    gtk_widget_add_controller(canvas_, GTK_EVENT_CONTROLLER(drag));

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(
        motion,
        "motion",
        G_CALLBACK(+[](
            GtkEventControllerMotion*,
            double x,
            double y,
            gpointer data
        ) {
            static_cast<WorkspaceOverviewOverlay*>(data)->handle_hover(x, y);
        }),
        this
    );
    gtk_widget_add_controller(canvas_, motion);

    gtk_window_set_child(window_, canvas_);
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

WorkspaceOverviewOverlay::~WorkspaceOverviewOverlay() {
    stop_animation(false);
    reset_drag();
    overview_canvas_clear(canvas_);
    release_assets();
    release_separator_nodes();
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
        window_ = nullptr;
    }
    canvas_ = nullptr;
}

void WorkspaceOverviewOverlay::initialize_separator_nodes() {
    for (std::size_t boundary = 0; boundary < separator_nodes_.size(); ++boundary) {
        separator_nodes_[boundary] = create_separator_node(boundary);
    }
}

void WorkspaceOverviewOverlay::release_separator_nodes() noexcept {
    for (auto*& node : separator_nodes_) {
        if (node != nullptr) {
            gsk_render_node_unref(node);
            node = nullptr;
        }
    }
}

void WorkspaceOverviewOverlay::reset_drag() noexcept {
    g_clear_object(&drag_card_.assets.compact);
    g_clear_object(&drag_card_.assets.expanded);
    drag_card_ = DragCard{};
    drag_target_index_ = -1;
    if (canvas_ != nullptr) {
        gtk_widget_set_cursor_from_name(canvas_, "default");
    }
}

void WorkspaceOverviewOverlay::finish_card_transition() noexcept {
    card_animation_active_ = false;
    card_animation_start_time_us_ = 0;
    card_animation_progress_ = 1.0;
    for (std::size_t realm = 0; realm < card_from_slots_.size(); ++realm) {
        for (std::size_t slot = 0; slot < card_from_slots_[realm].size(); ++slot) {
            card_from_slots_[realm][slot] = static_cast<int>(slot);
            card_entering_[realm][slot] = false;
            auto& outgoing = outgoing_cards_[realm][slot];
            g_clear_object(&outgoing.assets.compact);
            g_clear_object(&outgoing.assets.expanded);
            outgoing.slot = 0;
            outgoing.active = false;
        }
    }
}

void WorkspaceOverviewOverlay::ensure_animation_tick() {
    if (animation_tick_id_ != 0 || canvas_ == nullptr) return;
    animation_tick_id_ = gtk_widget_add_tick_callback(
        canvas_,
        &WorkspaceOverviewOverlay::animation_tick_callback,
        this,
        nullptr
    );
}

void WorkspaceOverviewOverlay::prepare_card_transition(
    const WorkspaceOverviewState& next
) {
    finish_card_transition();
    if (!visible() || !assets_attempted_ || !asset_error_.empty()) return;

    bool has_motion = false;
    for (std::size_t realm = 0; realm < workspace_state_.size(); ++realm) {
        const auto& previous_realm = workspace_state_[realm];
        const auto& next_realm = next[realm];

        for (std::size_t slot = 0; slot < next_realm.card_count; ++slot) {
            const auto previous_slot = find_card_slot(
                previous_realm,
                next_realm.cards[slot]
            );
            if (previous_slot.has_value()) {
                card_from_slots_[realm][slot] =
                    static_cast<int>(*previous_slot);
                has_motion = has_motion || *previous_slot != slot;
            } else {
                card_from_slots_[realm][slot] = static_cast<int>(slot);
                card_entering_[realm][slot] = true;
                has_motion = true;
            }
        }

        std::size_t outgoing_index = 0;
        for (std::size_t slot = 0; slot < previous_realm.card_count; ++slot) {
            if (find_card_slot(next_realm, previous_realm.cards[slot]).has_value()) {
                continue;
            }
            if (outgoing_index >= outgoing_cards_[realm].size()) break;

            auto& outgoing = outgoing_cards_[realm][outgoing_index++];
            const auto& previous_assets = assets_[realm].cards[slot];
            if (previous_assets.compact != nullptr) {
                outgoing.assets.compact = GDK_TEXTURE(
                    g_object_ref(previous_assets.compact)
                );
            }
            if (previous_assets.expanded != nullptr) {
                outgoing.assets.expanded = GDK_TEXTURE(
                    g_object_ref(previous_assets.expanded)
                );
            }
            outgoing.slot = slot;
            outgoing.active = outgoing.assets.compact != nullptr ||
                outgoing.assets.expanded != nullptr;
            has_motion = has_motion || outgoing.active;
        }
    }

    if (!has_motion) return;
    card_animation_active_ = true;
    card_animation_progress_ = 0.0;
    card_animation_start_time_us_ = 0;
    ensure_animation_tick();
}

void WorkspaceOverviewOverlay::synchronize_active_workspace() {
    for (std::size_t index = 0; index < workspace_state_.size(); ++index) {
        if (!workspace_state_[index].active) continue;
        active_index_ = static_cast<int>(index);
        stop_animation(false);
        displayed_heights_ = target_realm_heights(active_index_);
        animation_start_heights_ = displayed_heights_;
        animation_target_heights_ = displayed_heights_;
        return;
    }
}

bool WorkspaceOverviewOverlay::rebuild_dirty_overlays() {
    bool success = true;
    for (std::size_t index = 0; index < assets_.size(); ++index) {
        if (!overlay_dirty_[index]) continue;
        if (assets_[index].background == nullptr ||
            assets_[index].character == nullptr ||
            assets_[index].roman_layout == nullptr ||
            assets_[index].element_layout == nullptr ||
            assets_[index].place_layout == nullptr) {
            success = false;
            continue;
        }

        std::string render_error;
        GdkTexture* compact = assets_[index].compact_overlay != nullptr
            ? GDK_TEXTURE(g_object_ref(assets_[index].compact_overlay))
            : render_realm_overlay_texture(
                index,
                workspace_state_[index],
                false,
                render_error
            );
        GdkTexture* expanded = assets_[index].expanded_overlay != nullptr
            ? GDK_TEXTURE(g_object_ref(assets_[index].expanded_overlay))
            : compact != nullptr
                ? render_realm_overlay_texture(
                    index,
                    workspace_state_[index],
                    true,
                    render_error
                )
                : nullptr;
        std::array<CardAssets, kWorkspaceOverviewCardLimit> cards{};
        bool cards_ready = compact != nullptr && expanded != nullptr;
        for (std::size_t slot = 0;
             cards_ready && slot < workspace_state_[index].card_count;
             ++slot) {
            cards[slot].compact = render_card_texture(
                index,
                workspace_state_[index].cards[slot],
                slot,
                false,
                render_error
            );
            cards[slot].expanded = cards[slot].compact != nullptr
                ? render_card_texture(
                    index,
                    workspace_state_[index].cards[slot],
                    slot,
                    true,
                    render_error
                )
                : nullptr;
            cards_ready = cards[slot].compact != nullptr &&
                cards[slot].expanded != nullptr;
        }

        if (!cards_ready) {
            g_clear_object(&compact);
            g_clear_object(&expanded);
            for (auto& card : cards) {
                g_clear_object(&card.compact);
                g_clear_object(&card.expanded);
            }
            std::cerr
                << "[WorkspaceOverview] unable to refresh workspace "
                << workspace_state_[index].workspace_id
                << " cards";
            if (!render_error.empty()) std::cerr << ": " << render_error;
            std::cerr << '\n';
            success = false;
            continue;
        }

        g_clear_object(&assets_[index].compact_overlay);
        g_clear_object(&assets_[index].expanded_overlay);
        for (auto& card : assets_[index].cards) {
            g_clear_object(&card.compact);
            g_clear_object(&card.expanded);
        }
        assets_[index].compact_overlay = compact;
        assets_[index].expanded_overlay = expanded;
        assets_[index].cards = cards;
        overlay_dirty_[index] = false;
    }
    return success;
}

void WorkspaceOverviewOverlay::set_workspace_snapshot(
    const services::WorkspaceSnapshot& snapshot
) {
    auto next = build_workspace_overview_state(snapshot);
    bool cards_changed = false;
    for (std::size_t index = 0; index < next.size(); ++index) {
        if (!same_workspace_overview_cards(workspace_state_[index], next[index])) {
            overlay_dirty_[index] = true;
            cards_changed = true;
        }
    }
    if (cards_changed) prepare_card_transition(next);
    workspace_state_ = std::move(next);

    if (!visible()) synchronize_active_workspace();
    if (visible() && assets_attempted_ && asset_error_.empty()) {
        static_cast<void>(rebuild_dirty_overlays());
    }
    if (canvas_ != nullptr) gtk_widget_queue_draw(canvas_);
}

void WorkspaceOverviewOverlay::show() {
    synchronize_active_workspace();
    static_cast<void>(ensure_assets());
    static_cast<void>(rebuild_dirty_overlays());
    gtk_widget_queue_draw(canvas_);
    gtk_window_present(window_);
    gtk_widget_grab_focus(canvas_);
}

void WorkspaceOverviewOverlay::hide() {
    reset_drag();
    stop_animation(true);
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

void WorkspaceOverviewOverlay::toggle() {
    if (visible()) hide(); else show();
}

bool WorkspaceOverviewOverlay::visible() const {
    return window_ != nullptr && gtk_widget_get_visible(GTK_WIDGET(window_));
}

void WorkspaceOverviewOverlay::snapshot_callback(
    GtkWidget* widget,
    GtkSnapshot* snapshot,
    gpointer data
) {
    static_cast<WorkspaceOverviewOverlay*>(data)->snapshot(widget, snapshot);
}

gboolean WorkspaceOverviewOverlay::animation_tick_callback(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer data
) {
    auto* self = static_cast<WorkspaceOverviewOverlay*>(data);
    const gint64 frame_time_us = gdk_frame_clock_get_frame_time(frame_clock);
    bool keep_running = false;

    if (self->realm_animation_active_) {
        if (self->animation_start_time_us_ == 0) {
            self->animation_start_time_us_ = frame_time_us;
        }
        const double elapsed_seconds = static_cast<double>(
            frame_time_us - self->animation_start_time_us_
        ) / static_cast<double>(kMicrosecondsPerSecond);
        const double linear = std::clamp(
            elapsed_seconds / kTransitionDurationSeconds,
            0.0,
            1.0
        );
        const double eased = transition_ease(linear);
        for (std::size_t index = 0;
             index < self->displayed_heights_.size();
             ++index) {
            self->displayed_heights_[index] = lerp(
                self->animation_start_heights_[index],
                self->animation_target_heights_[index],
                eased
            );
        }
        if (linear >= 1.0) {
            self->displayed_heights_ = self->animation_target_heights_;
            self->animation_start_time_us_ = 0;
            self->realm_animation_active_ = false;
        } else {
            keep_running = true;
        }
    }

    if (self->card_animation_active_) {
        if (self->card_animation_start_time_us_ == 0) {
            self->card_animation_start_time_us_ = frame_time_us;
        }
        const double elapsed_seconds = static_cast<double>(
            frame_time_us - self->card_animation_start_time_us_
        ) / static_cast<double>(kMicrosecondsPerSecond);
        const double linear = std::clamp(
            elapsed_seconds / kCardTransitionDurationSeconds,
            0.0,
            1.0
        );
        self->card_animation_progress_ = transition_ease(linear);
        if (linear >= 1.0) {
            self->finish_card_transition();
        } else {
            keep_running = true;
        }
    }

    gtk_widget_queue_draw(self->canvas_);
    if (!keep_running && !self->realm_animation_active_ &&
        !self->card_animation_active_) {
        self->animation_tick_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void WorkspaceOverviewOverlay::select_realm(int index) {
    if (index < 0 || index >= static_cast<int>(displayed_heights_.size())) return;

    // Motion events arrive continuously while the pointer moves inside a realm.
    // Do not restart an in-flight transition that is already targeting it.
    if (index == active_index_) return;

    active_index_ = index;
    const auto target = target_realm_heights(index);
    animation_start_heights_ = displayed_heights_;
    animation_target_heights_ = target;
    animation_start_time_us_ = 0;
    realm_animation_active_ = true;
    ensure_animation_tick();
}

void WorkspaceOverviewOverlay::stop_animation(bool snap_to_target) noexcept {
    if (animation_tick_id_ != 0 && canvas_ != nullptr) {
        gtk_widget_remove_tick_callback(canvas_, animation_tick_id_);
        animation_tick_id_ = 0;
    }
    animation_start_time_us_ = 0;
    realm_animation_active_ = false;
    finish_card_transition();
    if (snap_to_target) {
        displayed_heights_ = animation_target_heights_;
        animation_start_heights_ = animation_target_heights_;
    }
}

void WorkspaceOverviewOverlay::snapshot(
    GtkWidget* widget,
    GtkSnapshot* snapshot
) {
    const int width = gtk_widget_get_width(widget);
    const int height = gtk_widget_get_height(widget);
    if (width <= 0 || height <= 0) return;

    const graphene_rect_t widget_bounds = GRAPHENE_RECT_INIT(
        0.0F,
        0.0F,
        static_cast<float>(width),
        static_cast<float>(height)
    );
    const GdkRGBA black{0.0, 0.0, 0.0, 1.0};
    gtk_snapshot_append_color(snapshot, &black, &widget_bounds);

    const double scale_x = static_cast<double>(width) / kReferenceWidth;
    const double scale_y = static_cast<double>(height) / kReferenceHeight;

    gtk_snapshot_save(snapshot);
    gtk_snapshot_scale(
        snapshot,
        static_cast<float>(scale_x),
        static_cast<float>(scale_y)
    );

    const graphene_rect_t stage_bounds = GRAPHENE_RECT_INIT(
        0.0F,
        0.0F,
        static_cast<float>(kReferenceWidth),
        static_cast<float>(kReferenceHeight)
    );
    const GdkRGBA stage_color{0.008, 0.012, 0.016, 1.0};
    gtk_snapshot_append_color(snapshot, &stage_color, &stage_bounds);

    if (!ensure_assets()) {
        const std::string message = asset_error_.empty()
            ? "Workspace overview assets unavailable"
            : asset_error_;
        PangoLayout* layout = gtk_widget_create_pango_layout(
            widget,
            message.c_str()
        );
        PangoFontDescription* font = pango_font_description_from_string(
            "Inter Bold 24"
        );
        pango_layout_set_font_description(layout, font);
        pango_font_description_free(font);
        int text_width = 0;
        int text_height = 0;
        pango_layout_get_pixel_size(layout, &text_width, &text_height);
        const graphene_point_t text_offset = GRAPHENE_POINT_INIT(
            static_cast<float>(kReferenceWidth * 0.5 - text_width * 0.5),
            static_cast<float>(kReferenceHeight * 0.5 - text_height * 0.5)
        );
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &text_offset);
        const GdkRGBA error_color{0.95, 0.82, 0.55, 1.0};
        gtk_snapshot_append_layout(snapshot, layout, &error_color);
        gtk_snapshot_restore(snapshot);
        g_object_unref(layout);
        gtk_snapshot_restore(snapshot);
        return;
    }

    const auto& heights = displayed_heights_;
    const auto tops = realm_tops(heights);

    for (std::size_t index = 0; index < kRealms.size(); ++index) {
        const double realm_height = heights[index];
        const double activity = realm_activity(realm_height);
        const Rect texture_bounds{
            0.0,
            tops[index],
            kReferenceWidth,
            realm_height,
        };
        const graphene_rect_t realm_clip = GRAPHENE_RECT_INIT(
            0.0F,
            static_cast<float>(tops[index]),
            static_cast<float>(kReferenceWidth),
            static_cast<float>(realm_height)
        );
        gtk_snapshot_push_clip(snapshot, &realm_clip);
        append_texture_with_opacity(
            snapshot,
            assets_[index].background,
            background_bounds(
                assets_[index].background,
                tops[index],
                realm_height,
                activity
            ),
            1.0
        );
        append_realm_atmosphere(
            snapshot,
            tops[index],
            realm_height,
            activity
        );
        append_texture_with_opacity(
            snapshot,
            assets_[index].character,
            character_bounds(
                assets_[index].character,
                kRealms[index],
                tops[index],
                realm_height,
                activity
            ),
            lerp(0.92, 1.0, activity)
        );
        append_texture_with_opacity(
            snapshot,
            assets_[index].compact_overlay,
            texture_bounds,
            1.0 - activity
        );
        append_texture_with_opacity(
            snapshot,
            assets_[index].expanded_overlay,
            texture_bounds,
            activity
        );

        if (drag_card_.active &&
            drag_target_index_ == static_cast<int>(index)) {
            const GdkRGBA target_tint{
                static_cast<float>(kRealms[index].accent.red),
                static_cast<float>(kRealms[index].accent.green),
                static_cast<float>(kRealms[index].accent.blue),
                0.105F,
            };
            gtk_snapshot_append_color(snapshot, &target_tint, &realm_clip);

            const GdkRGBA target_edge{
                static_cast<float>(kRealms[index].accent_soft.red),
                static_cast<float>(kRealms[index].accent_soft.green),
                static_cast<float>(kRealms[index].accent_soft.blue),
                0.46F,
            };
            const graphene_rect_t top_edge = GRAPHENE_RECT_INIT(
                0.0F,
                static_cast<float>(tops[index] + 2.0),
                static_cast<float>(kReferenceWidth),
                2.0F
            );
            const graphene_rect_t bottom_edge = GRAPHENE_RECT_INIT(
                0.0F,
                static_cast<float>(tops[index] + realm_height - 4.0),
                static_cast<float>(kReferenceWidth),
                2.0F
            );
            gtk_snapshot_append_color(snapshot, &target_edge, &top_edge);
            gtk_snapshot_append_color(snapshot, &target_edge, &bottom_edge);
        }

        const double card_progress = card_animation_active_
            ? card_animation_progress_
            : 1.0;
        const auto card_count = std::min(
            workspace_state_[index].card_count,
            assets_[index].cards.size()
        );
        for (std::size_t slot = 0; slot < card_count; ++slot) {
            const int configured_from = card_from_slots_[index][slot];
            const std::size_t from_slot = configured_from >= 0 &&
                configured_from < static_cast<int>(kWorkspaceOverviewCardLimit)
                ? static_cast<std::size_t>(configured_from)
                : slot;
            const bool entering = card_animation_active_ &&
                card_entering_[index][slot];

            const Rect compact_target = scaled_card_rect(
                slot,
                false,
                tops[index],
                realm_height
            );
            const Rect expanded_target = scaled_card_rect(
                slot,
                true,
                tops[index],
                realm_height
            );
            const Rect compact_start = entering
                ? translated_rect(compact_target, -kCardTransitionDistance)
                : scaled_card_rect(
                    from_slot,
                    false,
                    tops[index],
                    realm_height
                );
            const Rect expanded_start = entering
                ? translated_rect(expanded_target, -kCardTransitionDistance)
                : scaled_card_rect(
                    from_slot,
                    true,
                    tops[index],
                    realm_height
                );
            const bool dragged_source = drag_card_.active &&
                drag_card_.realm_index == index &&
                drag_card_.card_index == slot;
            const double card_opacity = entering ? card_progress : 1.0;
            append_card_texture_pair(
                snapshot,
                assets_[index].cards[slot].compact,
                assets_[index].cards[slot].expanded,
                lerp_rect(compact_start, compact_target, card_progress),
                lerp_rect(expanded_start, expanded_target, card_progress),
                activity,
                dragged_source ? card_opacity * 0.14 : card_opacity
            );
        }

        if (card_animation_active_) {
            for (const auto& outgoing : outgoing_cards_[index]) {
                if (!outgoing.active) continue;
                const Rect compact_start = scaled_card_rect(
                    outgoing.slot,
                    false,
                    tops[index],
                    realm_height
                );
                const Rect expanded_start = scaled_card_rect(
                    outgoing.slot,
                    true,
                    tops[index],
                    realm_height
                );
                append_card_texture_pair(
                    snapshot,
                    outgoing.assets.compact,
                    outgoing.assets.expanded,
                    lerp_rect(
                        compact_start,
                        translated_rect(
                            compact_start,
                            kCardTransitionDistance
                        ),
                        card_progress
                    ),
                    lerp_rect(
                        expanded_start,
                        translated_rect(
                            expanded_start,
                            kCardTransitionDistance
                        ),
                        card_progress
                    ),
                    activity,
                    1.0 - card_progress
                );
            }
        }

        const Color identity_shadow{
            kRealms[index].accent.red,
            kRealms[index].accent.green,
            kRealms[index].accent.blue,
            0.22,
        };
        append_identity_layout(
            snapshot,
            assets_[index].roman_layout,
            112.0,
            tops[index] + realm_height * 0.5,
            kRealms[index].accent_soft,
            &identity_shadow,
            0.70
        );
        append_identity_layout(
            snapshot,
            assets_[index].element_layout,
            190.0,
            tops[index] + realm_height * 0.5 - 14.0,
            {1.0, 1.0, 1.0, 0.90},
            &identity_shadow,
            0.52
        );
        append_identity_layout(
            snapshot,
            assets_[index].place_layout,
            190.0,
            tops[index] + realm_height * 0.5 + 17.0,
            {1.0, 1.0, 1.0, 0.52},
            &identity_shadow,
            0.36
        );
        gtk_snapshot_pop(snapshot);
    }

    append_ripple_separator(snapshot, separator_nodes_[0], tops[1]);
    append_ripple_separator(snapshot, separator_nodes_[1], tops[2]);
    append_ripple_separator(snapshot, separator_nodes_[2], tops[3]);
    append_global_vignette(snapshot);

    if (drag_card_.active) {
        const double delta_x = drag_card_.current_x - drag_card_.start_x;
        const double delta_y = drag_card_.current_y - drag_card_.start_y;
        const Rect compact_origin{
            drag_card_.compact_bounds.x,
            drag_card_.compact_bounds.y,
            drag_card_.compact_bounds.width,
            drag_card_.compact_bounds.height,
        };
        const Rect expanded_origin{
            drag_card_.expanded_bounds.x,
            drag_card_.expanded_bounds.y,
            drag_card_.expanded_bounds.width,
            drag_card_.expanded_bounds.height,
        };
        append_card_texture_pair(
            snapshot,
            drag_card_.assets.compact,
            drag_card_.assets.expanded,
            scaled_around_center(
                translated_rect(compact_origin, delta_x, delta_y),
                kDraggedCardScale
            ),
            scaled_around_center(
                translated_rect(expanded_origin, delta_x, delta_y),
                kDraggedCardScale
            ),
            drag_card_.activity,
            0.97
        );
    }
    gtk_snapshot_restore(snapshot);
}

void WorkspaceOverviewOverlay::handle_drag_begin(double x, double y) {
    reset_drag();
    drag_card_.gesture_active = true;
    if (canvas_ == nullptr) return;

    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    if (width <= 0 || height <= 0) return;

    const double scale_x = static_cast<double>(width) / kReferenceWidth;
    const double scale_y = static_cast<double>(height) / kReferenceHeight;
    drag_card_.start_x = x / scale_x;
    drag_card_.start_y = y / scale_y;
    drag_card_.current_x = drag_card_.start_x;
    drag_card_.current_y = drag_card_.start_y;

    if (card_animation_active_) return;
    const auto hit = hit_realm_card(
        drag_card_.start_x,
        drag_card_.start_y,
        displayed_heights_,
        workspace_state_
    );
    if (!hit.has_value()) return;

    const auto& realm = workspace_state_[hit->realm_index];
    const auto& card = realm.cards[hit->card_index];
    if (card.summary || card.address.empty()) return;

    const auto tops = realm_tops(displayed_heights_);
    const double realm_height = displayed_heights_[hit->realm_index];
    const Rect compact = scaled_card_rect(
        hit->card_index,
        false,
        tops[hit->realm_index],
        realm_height
    );
    const Rect expanded = scaled_card_rect(
        hit->card_index,
        true,
        tops[hit->realm_index],
        realm_height
    );
    const auto& source_assets = assets_[hit->realm_index].cards[hit->card_index];
    if (source_assets.compact == nullptr && source_assets.expanded == nullptr) return;

    if (source_assets.compact != nullptr) {
        drag_card_.assets.compact = GDK_TEXTURE(g_object_ref(source_assets.compact));
    }
    if (source_assets.expanded != nullptr) {
        drag_card_.assets.expanded = GDK_TEXTURE(g_object_ref(source_assets.expanded));
    }
    drag_card_.compact_bounds = {
        compact.x, compact.y, compact.width, compact.height,
    };
    drag_card_.expanded_bounds = {
        expanded.x, expanded.y, expanded.width, expanded.height,
    };
    drag_card_.address = card.address;
    drag_card_.realm_index = hit->realm_index;
    drag_card_.card_index = hit->card_index;
    drag_card_.activity = realm_activity(realm_height);
    drag_card_.armed = true;
}

void WorkspaceOverviewOverlay::handle_drag_update(
    double offset_x,
    double offset_y
) {
    if (!drag_card_.gesture_active || !drag_card_.armed || canvas_ == nullptr) {
        return;
    }

    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    if (width <= 0 || height <= 0) return;

    if (!drag_card_.active &&
        std::hypot(offset_x, offset_y) < kCardDragThresholdPixels) {
        return;
    }
    if (!drag_card_.active) {
        drag_card_.active = true;
        gtk_widget_set_cursor_from_name(canvas_, "grabbing");
    }

    const double scale_x = static_cast<double>(width) / kReferenceWidth;
    const double scale_y = static_cast<double>(height) / kReferenceHeight;
    drag_card_.current_x = drag_card_.start_x + offset_x / scale_x;
    drag_card_.current_y = drag_card_.start_y + offset_y / scale_y;

    const auto target = realm_index_at_point(
        drag_card_.current_x,
        drag_card_.current_y,
        displayed_heights_
    );
    if (target.has_value() &&
        *target != static_cast<int>(drag_card_.realm_index)) {
        drag_target_index_ = *target;
        select_realm(*target);
    } else {
        drag_target_index_ = -1;
    }
    gtk_widget_queue_draw(canvas_);
}

void WorkspaceOverviewOverlay::handle_drag_end(
    double offset_x,
    double offset_y
) {
    if (!drag_card_.gesture_active || canvas_ == nullptr) return;

    const double distance = std::hypot(offset_x, offset_y);
    if (drag_card_.armed && !drag_card_.active &&
        distance >= kCardDragThresholdPixels) {
        handle_drag_update(offset_x, offset_y);
    }

    if (drag_card_.active) {
        handle_drag_update(offset_x, offset_y);
        const int target_index = drag_target_index_;
        const std::size_t source_index = drag_card_.realm_index;
        const std::string address = drag_card_.address;
        const int destination_workspace = target_index >= 0
            ? workspace_id_for_realm_index(
                static_cast<std::size_t>(target_index)
            )
            : 0;
        reset_drag();
        gtk_widget_queue_draw(canvas_);

        if (target_index >= 0 &&
            static_cast<std::size_t>(target_index) != source_index &&
            destination_workspace > 0 && !address.empty() && move_window_) {
            move_window_(destination_workspace, address);
        }
        return;
    }

    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    const double scale_x = width > 0
        ? static_cast<double>(width) / kReferenceWidth
        : 1.0;
    const double scale_y = height > 0
        ? static_cast<double>(height) / kReferenceHeight
        : 1.0;
    const double end_x = drag_card_.start_x * scale_x + offset_x;
    const double end_y = drag_card_.start_y * scale_y + offset_y;
    reset_drag();
    if (distance < kCardDragThresholdPixels) {
        handle_primary_click(end_x, end_y);
    }
}

void WorkspaceOverviewOverlay::handle_primary_click(double x, double y) {
    if (canvas_ == nullptr) return;
    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    if (width <= 0 || height <= 0) return;

    const double scale_x = static_cast<double>(width) / kReferenceWidth;
    const double scale_y = static_cast<double>(height) / kReferenceHeight;
    const double reference_x = x / scale_x;
    const double reference_y = y / scale_y;
    const auto card_realm = hit_realm_card(
        reference_x,
        reference_y,
        displayed_heights_,
        workspace_state_
    );
    if (card_realm.has_value()) {
        const auto& realm = workspace_state_[card_realm->realm_index];
        const auto& card = realm.cards[card_realm->card_index];
        if (!card.summary && !card.address.empty() && activate_window_) {
            activate_window_(realm.workspace_id, card.address);
        } else if (activate_workspace_) {
            activate_workspace_(realm.workspace_id);
        }
        hide();
        return;
    }

    const auto identity_realm = hit_realm_identity(
        reference_x,
        reference_y,
        displayed_heights_
    );
    if (identity_realm.has_value()) {
        if (*identity_realm != active_index_) select_realm(*identity_realm);
        return;
    }
    hide();
}

void WorkspaceOverviewOverlay::handle_hover(double, double y) {
    if (canvas_ == nullptr || drag_card_.active) return;
    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    if (width <= 0 || height <= 0) return;

    const double scale_y = static_cast<double>(height) / kReferenceHeight;
    const double reference_y = y / scale_y;
    double bottom = 0.0;
    for (std::size_t index = 0; index < displayed_heights_.size(); ++index) {
        bottom += displayed_heights_[index];
        if (reference_y < bottom) {
            select_realm(static_cast<int>(index));
            return;
        }
    }
}

bool WorkspaceOverviewOverlay::ensure_assets() {
    if (assets_attempted_) return asset_error_.empty();
    assets_attempted_ = true;

    for (std::size_t index = 0; index < kRealms.size(); ++index) {
        const auto background_path = resolve_project_asset(
            kRealms[index].background_asset
        );
        const auto character_path = resolve_project_asset(
            kRealms[index].character_asset
        );
        if (!background_path || !character_path) {
            asset_error_ = "Unable to resolve workspace overview assets";
            std::cerr << "[WorkspaceOverview] " << asset_error_ << '\n';
            release_assets();
            return false;
        }

        assets_[index].background = load_texture(
            *background_path,
            asset_error_
        );
        if (assets_[index].background != nullptr) {
            assets_[index].character = load_texture(
                *character_path,
                asset_error_
            );
        }
        if (assets_[index].character != nullptr) {
            assets_[index].roman_layout = create_identity_layout(
                canvas_,
                kRealms[index].roman,
                60.0,
                "Georgia",
                0,
                asset_error_
            );
        }
        if (assets_[index].roman_layout != nullptr) {
            assets_[index].element_layout = create_identity_layout(
                canvas_,
                kRealms[index].element,
                23.0,
                "Georgia",
                3,
                asset_error_
            );
        }
        if (assets_[index].element_layout != nullptr) {
            assets_[index].place_layout = create_identity_layout(
                canvas_,
                kRealms[index].place,
                12.0,
                "Inter",
                2,
                asset_error_
            );
        }
        if (assets_[index].background == nullptr ||
            assets_[index].character == nullptr ||
            assets_[index].roman_layout == nullptr ||
            assets_[index].element_layout == nullptr ||
            assets_[index].place_layout == nullptr) {
            if (asset_error_.empty()) {
                asset_error_ = "Unable to prepare workspace overview layers";
            }
            std::cerr << "[WorkspaceOverview] " << asset_error_ << '\n';
            release_assets();
            return false;
        }
    }

    if (!rebuild_dirty_overlays()) {
        asset_error_ = "Unable to prepare workspace overview card overlays";
        release_assets();
        return false;
    }

    std::cerr
        << "[WorkspaceOverview] backgrounds, characters, and live card overlays cached\n";
    return true;
}

void WorkspaceOverviewOverlay::release_assets() noexcept {
    finish_card_transition();
    overlay_dirty_.fill(true);
    for (auto& realm : assets_) {
        g_clear_object(&realm.background);
        g_clear_object(&realm.character);
        g_clear_object(&realm.roman_layout);
        g_clear_object(&realm.element_layout);
        g_clear_object(&realm.place_layout);
        g_clear_object(&realm.compact_overlay);
        g_clear_object(&realm.expanded_overlay);
        for (auto& card : realm.cards) {
            g_clear_object(&card.compact);
            g_clear_object(&card.expanded);
        }
    }
}

} // namespace realmheart::ui::workspace
