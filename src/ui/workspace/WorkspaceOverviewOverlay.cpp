#include "ui/workspace/WorkspaceOverviewOverlay.hpp"
#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/BarGeometry.hpp"
#include "ui/workspace/animation/WorkspaceMorphFrontier.hpp"
#include "ui/workspace/animation/WorkspaceMorphPresentation.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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

void install_workspace_overview_transparency(GtkWidget* widget) {
    if (widget == nullptr) return;
    GdkDisplay* display = gtk_widget_get_display(widget);
    if (display == nullptr) return;

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        provider,
        animation::kWorkspaceOverviewTransparentCss.data()
    );
    gtk_style_context_add_provider_for_display(
        display,
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 1U
    );
    g_object_unref(provider);
}

[[nodiscard]] double monotonic_seconds() noexcept {
    return static_cast<double>(g_get_monotonic_time()) /
        static_cast<double>(G_USEC_PER_SEC);
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
constexpr double kOverflowTransitionDurationSeconds = 0.200;
constexpr double kSelectionTransitionDurationSeconds = 0.140;
constexpr double kViewportTransitionDurationSeconds = 0.170;
constexpr double kViewportTransitionDistance = 72.0;
constexpr double kCardTransitionDistance = 18.0;
constexpr double kCardDragThresholdPixels = 8.0;
constexpr double kCompactCardRadius = 11.0;
constexpr double kExpandedCardRadius = 15.0;
constexpr int kDefaultCardSelection = -2;
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


const RealmStyle& realm_style_for_workspace(int workspace_id) {
    return kRealms[style_index_for_workspace_id(workspace_id)];
}

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

graphene_rect_t rect_to_graphene(const Rect& rect) {
    const graphene_rect_t result = GRAPHENE_RECT_INIT(
        static_cast<float>(rect.x),
        static_cast<float>(rect.y),
        static_cast<float>(rect.width),
        static_cast<float>(rect.height)
    );
    return result;
}

Rect rect_from_graphene(const graphene_rect_t& rect) {
    return {
        static_cast<double>(rect.origin.x),
        static_cast<double>(rect.origin.y),
        static_cast<double>(rect.size.width),
        static_cast<double>(rect.size.height),
    };
}

graphene_rect_t lerp_graphene_rect(
    const graphene_rect_t& from,
    const graphene_rect_t& to,
    double progress
) {
    return rect_to_graphene(lerp_rect(
        rect_from_graphene(from),
        rect_from_graphene(to),
        progress
    ));
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

[[nodiscard]] GskPath* create_workspace_frontier_reveal_path(
    const animation::WorkspaceMorphFrontier& frontier
) {
    if (frontier.points.empty()) return nullptr;
    GskPathBuilder* builder = gsk_path_builder_new();
    gsk_path_builder_move_to(
        builder,
        static_cast<float>(frontier.reveal_left_x),
        static_cast<float>(frontier.points.front().y)
    );
    for (const auto& point : frontier.points) {
        gsk_path_builder_line_to(
            builder,
            static_cast<float>(point.x),
            static_cast<float>(point.y)
        );
    }
    gsk_path_builder_line_to(
        builder,
        static_cast<float>(frontier.reveal_left_x),
        static_cast<float>(frontier.points.back().y)
    );
    gsk_path_builder_close(builder);
    return gsk_path_builder_free_to_path(builder);
}

[[nodiscard]] GskPath* create_workspace_frontier_band_path(
    const animation::WorkspaceMorphFrontier& frontier,
    double half_width
) {
    if (frontier.points.empty() || half_width <= 0.0) return nullptr;

    // A blunt closed band leaves a horizontal cap at the top and bottom of
    // every realm frontier. Once blurred, those caps read as the tiny pale
    // separator bars visible during Opening/Closing. Taper both ends to a
    // single point instead: the travelling edge stays just as bright, but
    // there is no horizontal segment for the blur to illuminate.
    const auto& first = frontier.points.front();
    const auto& last = frontier.points.back();
    const double taper = std::max(half_width * 1.35, 2.0);

    GskPathBuilder* builder = gsk_path_builder_new();
    gsk_path_builder_move_to(
        builder,
        static_cast<float>(first.x),
        static_cast<float>(first.y - taper)
    );
    gsk_path_builder_line_to(
        builder,
        static_cast<float>(first.x - half_width),
        static_cast<float>(first.y)
    );
    for (std::size_t index = 1; index < frontier.points.size(); ++index) {
        const auto& point = frontier.points[index];
        gsk_path_builder_line_to(
            builder,
            static_cast<float>(point.x - half_width),
            static_cast<float>(point.y)
        );
    }
    gsk_path_builder_line_to(
        builder,
        static_cast<float>(last.x),
        static_cast<float>(last.y + taper)
    );
    for (std::size_t index = frontier.points.size(); index > 0; --index) {
        const auto& point = frontier.points[index - 1U];
        gsk_path_builder_line_to(
            builder,
            static_cast<float>(point.x + half_width),
            static_cast<float>(point.y)
        );
    }
    gsk_path_builder_close(builder);
    return gsk_path_builder_free_to_path(builder);
}

void append_workspace_frontier_particles(
    GtkSnapshot* snapshot,
    const animation::WorkspaceMorphFrontier& frontier,
    const RealmStyle& style,
    std::size_t style_index
) {
    for (std::size_t index = 0; index < frontier.particles.size(); ++index) {
        const auto& particle = frontier.particles[index];
        if (particle.opacity <= 0.001 || particle.width <= 0.0 ||
            particle.height <= 0.0) {
            continue;
        }
        const GdkRGBA color{
            static_cast<float>(index % 3U == 0U
                ? style.accent_soft.red
                : style.accent.red),
            static_cast<float>(index % 3U == 0U
                ? style.accent_soft.green
                : style.accent.green),
            static_cast<float>(index % 3U == 0U
                ? style.accent_soft.blue
                : style.accent.blue),
            static_cast<float>(particle.opacity),
        };

        if (style_index % 4U == 3U && index % 3U == 0U) {
            GskPathBuilder* builder = gsk_path_builder_new();
            gsk_path_builder_move_to(
                builder,
                static_cast<float>(particle.x),
                static_cast<float>(particle.y - particle.height * 0.5)
            );
            gsk_path_builder_line_to(
                builder,
                static_cast<float>(particle.x + particle.width * 0.65),
                static_cast<float>(particle.y)
            );
            gsk_path_builder_line_to(
                builder,
                static_cast<float>(particle.x),
                static_cast<float>(particle.y + particle.height * 0.5)
            );
            gsk_path_builder_line_to(
                builder,
                static_cast<float>(particle.x - particle.width * 0.35),
                static_cast<float>(particle.y)
            );
            gsk_path_builder_close(builder);
            GskPath* shard = gsk_path_builder_free_to_path(builder);
            gtk_snapshot_append_fill(
                snapshot,
                shard,
                GSK_FILL_RULE_WINDING,
                &color
            );
            gsk_path_unref(shard);
            continue;
        }

        const graphene_rect_t bounds = GRAPHENE_RECT_INIT(
            static_cast<float>(particle.x),
            static_cast<float>(particle.y - particle.height * 0.5),
            static_cast<float>(particle.width),
            static_cast<float>(particle.height)
        );
        gtk_snapshot_append_color(snapshot, &color, &bounds);
    }
}

void append_workspace_native_frontier(
    GtkSnapshot* snapshot,
    const animation::WorkspaceMorphFrontier& frontier,
    const RealmStyle& style,
    std::size_t style_index
) {
    if (frontier.glow_opacity <= 0.001 &&
        frontier.core_opacity <= 0.001) {
        return;
    }

    GskPath* glow = create_workspace_frontier_band_path(
        frontier,
        frontier.glow_half_width
    );
    GskPath* core = create_workspace_frontier_band_path(
        frontier,
        frontier.core_half_width
    );
    GskPath* highlight = create_workspace_frontier_band_path(
        frontier,
        std::max(0.7, frontier.core_half_width * 0.34)
    );
    if (glow == nullptr || core == nullptr || highlight == nullptr) {
        if (glow != nullptr) gsk_path_unref(glow);
        if (core != nullptr) gsk_path_unref(core);
        if (highlight != nullptr) gsk_path_unref(highlight);
        return;
    }

    const GdkRGBA glow_color{
        static_cast<float>(style.accent.red),
        static_cast<float>(style.accent.green),
        static_cast<float>(style.accent.blue),
        static_cast<float>(frontier.glow_opacity * 0.48),
    };
    gtk_snapshot_push_blur(snapshot, 11.0F);
    gtk_snapshot_append_fill(
        snapshot,
        glow,
        GSK_FILL_RULE_WINDING,
        &glow_color
    );
    gtk_snapshot_pop(snapshot);

    const GdkRGBA core_color{
        static_cast<float>(style.accent.red),
        static_cast<float>(style.accent.green),
        static_cast<float>(style.accent.blue),
        static_cast<float>(frontier.core_opacity * 0.86),
    };
    gtk_snapshot_append_fill(
        snapshot,
        core,
        GSK_FILL_RULE_WINDING,
        &core_color
    );

    const GdkRGBA highlight_color{
        static_cast<float>(style.accent_soft.red),
        static_cast<float>(style.accent_soft.green),
        static_cast<float>(style.accent_soft.blue),
        static_cast<float>(frontier.core_opacity),
    };
    gtk_snapshot_append_fill(
        snapshot,
        highlight,
        GSK_FILL_RULE_WINDING,
        &highlight_color
    );

    gsk_path_unref(glow);
    gsk_path_unref(core);
    gsk_path_unref(highlight);
    append_workspace_frontier_particles(
        snapshot,
        frontier,
        style,
        style_index
    );
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

std::size_t overflow_grid_columns(std::size_t count) noexcept {
    if (count <= 4U) return 2U;
    if (count <= 6U) return 3U;
    if (count <= 8U) return 4U;
    if (count == 9U) return 3U;
    if (count <= 12U) return 4U;
    if (count <= 15U) return 5U;
    if (count <= 16U) return 4U;
    return std::clamp<std::size_t>(
        static_cast<std::size_t>(std::ceil(std::sqrt(
            static_cast<double>(count) * 1.65
        ))),
        4U,
        7U
    );
}

std::vector<Rect> overflow_card_rects(
    double top,
    double realm_height,
    std::size_t count
) {
    std::vector<Rect> result;
    if (count == 0U || realm_height <= 0.0) return result;

    constexpr double kLeft = 355.0;
    constexpr double kRight = 1270.0;
    constexpr double kTopInset = 68.0;
    constexpr double kBottomInset = 54.0;
    constexpr double kGapX = 18.0;
    constexpr double kGapY = 16.0;

    const std::size_t columns = std::min(overflow_grid_columns(count), count);
    const std::size_t rows = (count + columns - 1U) / columns;
    const double available_width = kRight - kLeft;
    const double available_height = std::max(
        96.0,
        realm_height - kTopInset - kBottomInset
    );
    const double card_width = std::max(
        96.0,
        (available_width - kGapX * static_cast<double>(columns - 1U)) /
            static_cast<double>(columns)
    );
    const double card_height = std::max(
        52.0,
        (available_height - kGapY * static_cast<double>(rows - 1U)) /
            static_cast<double>(rows)
    );

    result.reserve(count);
    for (std::size_t row = 0; row < rows; ++row) {
        const std::size_t first = row * columns;
        const std::size_t row_count = std::min(columns, count - first);
        const double row_width = card_width * static_cast<double>(row_count) +
            kGapX * static_cast<double>(row_count - 1U);
        const double row_x = kLeft + (available_width - row_width) * 0.5;
        for (std::size_t column = 0; column < row_count; ++column) {
            result.push_back({
                row_x + static_cast<double>(column) * (card_width + kGapX),
                top + kTopInset + static_cast<double>(row) *
                    (card_height + kGapY),
                card_width,
                card_height,
            });
        }
    }
    return result;
}

Rect overflow_control_rect(double top) noexcept {
    return {1148.0, top + 22.0, 122.0, 34.0};
}

Rect overflow_card_current_rect(
    std::size_t slot,
    std::size_t count,
    double top,
    double realm_height,
    double activity,
    double progress
) {
    const auto targets = overflow_card_rects(top, realm_height, count);
    if (slot >= targets.size()) return {};
    const auto preview = window_card_visuals(top, realm_height, activity);
    const std::size_t origin_slot = slot < 2U ? slot : 2U;
    const Rect origin = preview[origin_slot].rect;
    return lerp_rect(origin, targets[slot], std::clamp(progress, 0.0, 1.0));
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
    bool overflow = false;
};

std::optional<WorkspaceCardHit> hit_realm_card(
    double x,
    double y,
    const std::array<double, 4>& heights,
    const WorkspaceOverviewState& workspace_state,
    int overflow_workspace_id,
    double overflow_progress
) {
    const auto tops = realm_tops(heights);
    for (std::size_t index = 0; index < kRealms.size(); ++index) {
        const auto& realm = workspace_state[index];
        const double activity = realm_activity(heights[index]);
        const bool overflow = realm.workspace_id == overflow_workspace_id &&
            realm.windows.size() > kWorkspaceOverviewPreviewCardLimit &&
            overflow_progress > 0.01;
        if (overflow) {
            for (std::size_t card_index = 0;
                 card_index < realm.windows.size();
                 ++card_index) {
                const Rect rect = overflow_card_current_rect(
                    card_index,
                    realm.windows.size(),
                    tops[index],
                    heights[index],
                    activity,
                    overflow_progress
                );
                const double opacity = card_index < 2U
                    ? 1.0
                    : std::clamp((overflow_progress - 0.06) / 0.54, 0.0, 1.0);
                if (opacity > 0.05 && rect.contains(x, y)) {
                    return WorkspaceCardHit{index, card_index, true};
                }
            }
            continue;
        }

        const auto cards = window_card_visuals(tops[index], heights[index], activity);
        const auto count = std::min(realm.card_count, cards.size());
        for (std::size_t card_index = 0; card_index < count; ++card_index) {
            const auto& card = cards[card_index];
            if (card.opacity > 0.05 && card.rect.contains(x, y)) {
                return WorkspaceCardHit{index, card_index, false};
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

std::string normalized_app_identifier(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0) {
            result.push_back(static_cast<char>(std::tolower(byte)));
        }
    }
    constexpr std::string_view suffix = "desktop";
    if (result.size() > suffix.size() &&
        result.compare(result.size() - suffix.size(), suffix.size(), suffix) == 0) {
        result.erase(result.size() - suffix.size());
    }
    return result;
}

void append_unique_icon_candidate(
    std::vector<std::string>& candidates,
    std::unordered_set<std::string>& seen,
    std::string value
) {
    if (value.empty() || !seen.insert(value).second) return;
    candidates.push_back(std::move(value));
}

std::vector<std::string> basic_icon_candidates(std::string_view requested) {
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seen;
    std::string raw(requested);
    append_unique_icon_candidate(candidates, seen, raw);

    if (raw.ends_with(".desktop")) {
        raw.resize(raw.size() - std::string_view(".desktop").size());
        append_unique_icon_candidate(candidates, seen, raw);
    }

    std::string lowered = raw;
    std::transform(
        lowered.begin(),
        lowered.end(),
        lowered.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    append_unique_icon_candidate(candidates, seen, lowered);

    const auto separator = lowered.find_last_of('.');
    if (separator != std::string::npos && separator + 1 < lowered.size()) {
        append_unique_icon_candidate(
            candidates,
            seen,
            lowered.substr(separator + 1)
        );
    }

    if (lowered.find("dolphin") != std::string::npos ||
        lowered.find("nautilus") != std::string::npos ||
        lowered.find("thunar") != std::string::npos) {
        append_unique_icon_candidate(candidates, seen, "system-file-manager");
    }
    if (lowered.find("terminal") != std::string::npos ||
        lowered.find("kitty") != std::string::npos ||
        lowered.find("alacritty") != std::string::npos) {
        append_unique_icon_candidate(candidates, seen, "utilities-terminal");
    }
    if (lowered == "code" || lowered.find("visualstudiocode") != std::string::npos) {
        append_unique_icon_candidate(candidates, seen, "visual-studio-code");
    }
    if (lowered.find("zen") != std::string::npos) {
        append_unique_icon_candidate(candidates, seen, "zen-browser");
    }
    return candidates;
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
    cairo_surface_t* icon_surface,
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

    const double content_top = y + titlebar_height;
    const double content_height = std::max(height - titlebar_height, 0.0);
    const double icon_size = std::clamp(
        std::min(width * 0.28, content_height * lerp(0.56, 0.48, detail)),
        18.0,
        88.0
    );
    const double icon_center_x = x + width * 0.5;
    const double icon_center_y = content_top + content_height * 0.5;

    set_source(cr, style.accent, lerp(0.08, 0.12, detail));
    cairo_arc(
        cr,
        icon_center_x,
        icon_center_y,
        icon_size * 0.72,
        0.0,
        2.0 * std::acos(-1.0)
    );
    cairo_fill(cr);

    if (icon_surface != nullptr &&
        cairo_surface_get_type(icon_surface) == CAIRO_SURFACE_TYPE_IMAGE) {
        const int source_width = cairo_image_surface_get_width(icon_surface);
        const int source_height = cairo_image_surface_get_height(icon_surface);
        if (source_width > 0 && source_height > 0) {
            const double scale = std::min(
                icon_size / static_cast<double>(source_width),
                icon_size / static_cast<double>(source_height)
            );
            const double draw_width = static_cast<double>(source_width) * scale;
            const double draw_height = static_cast<double>(source_height) * scale;
            cairo_save(cr);
            cairo_translate(
                cr,
                icon_center_x - draw_width * 0.5,
                icon_center_y - draw_height * 0.5
            );
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, icon_surface, 0.0, 0.0);
            cairo_paint_with_alpha(cr, 0.96);
            cairo_restore(cr);
        }
    } else {
        std::string monogram = "?";
        if (!content.first.empty()) {
            monogram.assign(1, static_cast<char>(std::toupper(
                static_cast<unsigned char>(content.first.front())
            )));
        }
        draw_text(
            cr,
            monogram,
            icon_center_x,
            icon_center_y,
            icon_size * 0.58,
            {0.96, 0.95, 0.92, 0.88},
            true,
            "Inter",
            true
        );
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
    cairo_surface_t* icon_surface,
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
        icon_surface,
        visual.detail,
        visual.opacity
    );

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    GdkTexture* texture = texture_from_surface(surface, error_message);
    cairo_surface_destroy(surface);
    return texture;
}

[[nodiscard]] GdkTexture* render_overflow_card_texture(
    std::size_t realm_index,
    const WorkspaceOverviewCard& content,
    cairo_surface_t* icon_surface,
    double width,
    double height,
    std::string& error_message
) {
    if (realm_index >= kRealms.size() || width <= 0.0 || height <= 0.0) {
        error_message = "Invalid overflow workspace card render request";
        return nullptr;
    }

    const int pixel_width = std::max(1, static_cast<int>(std::ceil(width)));
    const int pixel_height = std::max(1, static_cast<int>(std::ceil(height)));
    cairo_surface_t* surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32,
        pixel_width,
        pixel_height
    );
    const cairo_status_t surface_status = cairo_surface_status(surface);
    if (surface_status != CAIRO_STATUS_SUCCESS) {
        error_message = "Unable to allocate overflow workspace card surface: ";
        error_message += cairo_status_to_string(surface_status);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    cairo_t* cr = cairo_create(surface);
    const cairo_status_t context_status = cairo_status(cr);
    if (context_status != CAIRO_STATUS_SUCCESS) {
        error_message = "Unable to create overflow workspace card renderer: ";
        error_message += cairo_status_to_string(context_status);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return nullptr;
    }

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    const bool roomy = width >= 260.0;
    const std::string_view title = roomy
        ? std::string_view(content.title)
        : std::string_view{};
    draw_window_card(
        cr,
        0.0,
        0.0,
        width,
        height,
        kRealms[realm_index],
        {content.app_name, title},
        icon_surface,
        1.0,
        1.0
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
    std::size_t boundary_index,
    std::size_t boundary_style_index
) {
    constexpr std::array<std::pair<double, double>, 3> kLayerOffsets{{
        {-9.5, 9.5},
        {-7.4, 7.4},
        {5.4, 7.4},
    }};
    constexpr std::array<GdkRGBA, 4> kBodyColors{{
        {0.14F, 0.085F, 0.050F, 0.72F},
        {0.035F, 0.095F, 0.115F, 0.72F},
        {0.075F, 0.095F, 0.055F, 0.72F},
        {0.115F, 0.080F, 0.048F, 0.72F},
    }};
    constexpr std::array<GdkRGBA, 4> kLowerAccentColors{{
        {0.55F, 0.30F, 0.16F, 0.22F},
        {0.30F, 0.58F, 0.66F, 0.22F},
        {0.36F, 0.46F, 0.28F, 0.20F},
        {0.52F, 0.38F, 0.20F, 0.21F},
    }};
    if (boundary_index >= 3 ||
        boundary_style_index >= kWorkspaceOverviewRealmCount) {
        return nullptr;
    }
    const std::array<GdkRGBA, 3> colors{{
        {0.0F, 0.0F, 0.0F, 0.07F},
        kBodyColors[boundary_style_index],
        kLowerAccentColors[boundary_style_index],
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
    if (opacity <= 0.001) return;

    const double t = std::clamp(activity, 0.0, 1.0);
    const Rect current_bounds = lerp_rect(
        compact_bounds,
        expanded_bounds,
        t
    );

    // Never crossfade two differently-sized card textures during realm hover.
    // Their simultaneous resampling is the soft/ghosted "blur" visible while
    // a workspace expands or contracts. Pick the texture whose native state is
    // closest to the current geometry and scale exactly one image instead.
    GdkTexture* texture = t < 0.5 ? compact : expanded;
    if (texture == nullptr) texture = compact != nullptr ? compact : expanded;
    append_texture_with_opacity(
        snapshot,
        texture,
        current_bounds,
        opacity
    );
}

void append_card_selection_outline(
    GtkSnapshot* snapshot,
    const Rect& bounds,
    const Color& accent,
    double opacity,
    double radius
) {
    if (bounds.width <= 0.0 || bounds.height <= 0.0 || opacity <= 0.001) return;

    const graphene_rect_t border_bounds = rect_to_graphene(bounds);
    GskRoundedRect outline{};
    gsk_rounded_rect_init_from_rect(
        &outline,
        &border_bounds,
        static_cast<float>(std::max(0.0, radius))
    );

    const GdkRGBA glow{
        static_cast<float>(accent.red),
        static_cast<float>(accent.green),
        static_cast<float>(accent.blue),
        static_cast<float>(0.16 * opacity),
    };
    const GdkRGBA line{
        static_cast<float>(accent.red),
        static_cast<float>(accent.green),
        static_cast<float>(accent.blue),
        static_cast<float>(0.88 * opacity),
    };
    const std::array<float, 4> glow_widths{{4.0F, 4.0F, 4.0F, 4.0F}};
    const std::array<float, 4> line_widths{{1.5F, 1.5F, 1.5F, 1.5F}};
    const std::array<GdkRGBA, 4> glow_colors{{glow, glow, glow, glow}};
    const std::array<GdkRGBA, 4> line_colors{{line, line, line, line}};

    gtk_snapshot_append_border(
        snapshot,
        &outline,
        glow_widths.data(),
        glow_colors.data()
    );
    gtk_snapshot_append_border(
        snapshot,
        &outline,
        line_widths.data(),
        line_colors.data()
    );
}

void append_overflow_collapse_control(
    GtkWidget* widget,
    GtkSnapshot* snapshot,
    const Rect& bounds,
    const RealmStyle& style,
    double opacity
) {
    if (widget == nullptr || snapshot == nullptr || opacity <= 0.001) return;

    const graphene_rect_t rect = rect_to_graphene(bounds);
    GskRoundedRect rounded{};
    gsk_rounded_rect_init_from_rect(&rounded, &rect, 11.0F);
    gtk_snapshot_push_rounded_clip(snapshot, &rounded);
    const GdkRGBA fill{0.025F, 0.032F, 0.040F,
        static_cast<float>(0.90 * opacity)};
    gtk_snapshot_append_color(snapshot, &fill, &rect);
    gtk_snapshot_pop(snapshot);

    const GdkRGBA border{
        static_cast<float>(style.accent_soft.red),
        static_cast<float>(style.accent_soft.green),
        static_cast<float>(style.accent_soft.blue),
        static_cast<float>(0.58 * opacity),
    };
    const std::array<float, 4> widths{{1.0F, 1.0F, 1.0F, 1.0F}};
    const std::array<GdkRGBA, 4> colors{{border, border, border, border}};
    gtk_snapshot_append_border(
        snapshot,
        &rounded,
        widths.data(),
        colors.data()
    );

    PangoLayout* layout = gtk_widget_create_pango_layout(widget, "Collapse");
    if (layout == nullptr) return;
    PangoFontDescription* font = pango_font_description_from_string(
        "Inter SemiBold 12"
    );
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    int text_width = 0;
    int text_height = 0;
    pango_layout_get_pixel_size(layout, &text_width, &text_height);
    const graphene_point_t offset = GRAPHENE_POINT_INIT(
        static_cast<float>(
            bounds.x + (bounds.width - static_cast<double>(text_width)) * 0.5
        ),
        static_cast<float>(
            bounds.y + (bounds.height - static_cast<double>(text_height)) * 0.5
        )
    );
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &offset);
    const GdkRGBA text{0.96F, 0.95F, 0.92F,
        static_cast<float>(0.90 * opacity)};
    gtk_snapshot_append_layout(snapshot, layout, &text);
    gtk_snapshot_restore(snapshot);
    g_object_unref(layout);
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

void append_realm_horizontal_atmosphere(
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
}

void append_realm_atmosphere(
    GtkSnapshot* snapshot,
    double top,
    double height,
    double activity
) {
    if (height <= 0.0) return;

    // Keep the settled grading identical to the morph-safe grading. The old
    // per-realm vertical gradient reset at every boundary and was only enabled
    // after the morph reached its terminal frame. That created a broad
    // brightness jump on the exact frame the separators appeared, while also
    // reintroducing the same texture/atmosphere edge interaction we isolated
    // earlier. The wavy separator artwork already owns boundary depth, so the
    // atmosphere only needs the continuous left-to-right grade.
    append_realm_horizontal_atmosphere(snapshot, top, height, activity);
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
    std::function<void(int, std::string)> move_window,
    std::function<void(bool)> set_taskbar_morph_active,
    std::function<void(double)> set_taskbar_morph_progress
) : activate_workspace_(std::move(activate_workspace)),
    activate_window_(std::move(activate_window)),
    move_window_(std::move(move_window)),
    set_taskbar_morph_active_(std::move(set_taskbar_morph_active)),
    set_taskbar_morph_progress_(std::move(set_taskbar_morph_progress)) {
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
    gtk_widget_add_css_class(
        GTK_WIDGET(window_),
        "realmheart-workspace-overview-window"
    );
    gtk_widget_remove_css_class(GTK_WIDGET(window_), "background");
    install_workspace_overview_transparency(GTK_WIDGET(window_));
    morph_diagnostics_.set_enabled(
        animation::workspace_morph_diagnostics_enabled()
    );

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-workspace-overview";
    // The Aether Spine owns the overlay layer and must remain visible while
    // the realms materialize.  Keep the overview one layer below it while
    // still using the full-output coordinate space, so captured rune x/y
    // coordinates remain exact without allowing the overview to cover the bar.
    spec.layer = LayerSurfaceLevel::Top;
    spec.keyboard_mode = LayerKeyboardMode::OnDemand;
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    // The morph must share the monitor coordinate space with the Aether Spine.
    // A normal zone-0 layer surface is laid out inside the bar's 56 px
    // exclusive zone, which visibly kicks every captured rune to the right.
    // -1 asks layer-shell to use the full output, including reserved edges.
    spec.exclusive_zone = -1;
    spec.margin_left = 0;
    apply_layer_surface(window_, spec);

    canvas_ = overview_canvas_new(
        &WorkspaceOverviewOverlay::snapshot_callback,
        this
    );
    gtk_widget_set_hexpand(canvas_, TRUE);
    gtk_widget_set_vexpand(canvas_, TRUE);
    gtk_widget_set_focusable(canvas_, TRUE);
    gtk_widget_add_css_class(canvas_, "realmheart-workspace-overview-canvas");

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
            return self->handle_key_pressed(keyval)
                ? GDK_EVENT_STOP
                : GDK_EVENT_PROPAGATE;
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

    overlay_stack_ = gtk_overlay_new();
    gtk_widget_add_css_class(
        overlay_stack_,
        "realmheart-workspace-overview-stack"
    );
    gtk_widget_set_hexpand(overlay_stack_, TRUE);
    gtk_widget_set_vexpand(overlay_stack_, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay_stack_), canvas_);
    gtk_overlay_add_overlay(
        GTK_OVERLAY(overlay_stack_),
        morph_renderer_.widget()
    );
    gtk_overlay_set_measure_overlay(
        GTK_OVERLAY(overlay_stack_),
        morph_renderer_.widget(),
        FALSE
    );
    gtk_window_set_child(window_, overlay_stack_);
    set_morph_input_enabled(false);
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

WorkspaceOverviewOverlay::~WorkspaceOverviewOverlay() {
    if (set_taskbar_morph_progress_) set_taskbar_morph_progress_(0.0);
    if (taskbar_morph_active_ && set_taskbar_morph_active_) {
        set_taskbar_morph_active_(false);
        taskbar_morph_active_ = false;
    }
    stop_animation(false);
    morph_renderer_.finish();
    reset_drag();
    overview_canvas_clear(canvas_);
    release_assets();
    clear_icon_cache();
    release_separator_nodes();
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
        window_ = nullptr;
    }
    overlay_stack_ = nullptr;
    canvas_ = nullptr;
}

void WorkspaceOverviewOverlay::initialize_separator_nodes() {
    for (std::size_t boundary = 0; boundary < separator_nodes_.size(); ++boundary) {
        for (std::size_t style = 0;
             style < separator_nodes_[boundary].size();
             ++style) {
            separator_nodes_[boundary][style] = create_separator_node(
                boundary,
                style
            );
        }
    }
}

void WorkspaceOverviewOverlay::release_separator_nodes() noexcept {
    for (auto& boundary : separator_nodes_) {
        for (auto*& node : boundary) {
            if (node != nullptr) {
                gsk_render_node_unref(node);
                node = nullptr;
            }
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

bool WorkspaceOverviewOverlay::overflow_expanded_for(
    std::size_t realm_index
) const noexcept {
    return realm_index < workspace_state_.size() &&
        workspace_state_[realm_index].workspace_id == overflow_workspace_id_ &&
        workspace_state_[realm_index].windows.size() >
            kWorkspaceOverviewPreviewCardLimit &&
        (overflow_animation_progress_ > 0.001 ||
         overflow_animation_target_ > 0.5);
}

void WorkspaceOverviewOverlay::expand_overflow(std::size_t realm_index) {
    if (realm_index >= workspace_state_.size()) return;
    const auto& realm = workspace_state_[realm_index];
    if (realm.windows.size() <= kWorkspaceOverviewPreviewCardLimit) return;

    if (overflow_workspace_id_ != realm.workspace_id) {
        overflow_workspace_id_ = realm.workspace_id;
        overflow_animation_progress_ = 0.0;
    }
    overflow_animation_start_progress_ = overflow_animation_progress_;
    overflow_animation_target_ = 1.0;
    overflow_animation_start_time_us_ = 0;
    overflow_animation_active_ = true;

    const int preferred = static_cast<int>(std::min<std::size_t>(
        2U,
        realm.windows.size() - 1U
    ));
    transition_selection(static_cast<int>(realm_index), preferred);
    ensure_animation_tick();
    if (canvas_ != nullptr) gtk_widget_queue_draw(canvas_);
}

void WorkspaceOverviewOverlay::collapse_overflow(bool snap) noexcept {
    if (overflow_workspace_id_ == 0) return;
    if (snap) {
        overflow_workspace_id_ = 0;
        overflow_animation_progress_ = 0.0;
        overflow_animation_start_progress_ = 0.0;
        overflow_animation_target_ = 0.0;
        overflow_animation_start_time_us_ = 0;
        overflow_animation_active_ = false;
        normalize_selection();
        if (canvas_ != nullptr) gtk_widget_queue_draw(canvas_);
        return;
    }

    overflow_animation_start_progress_ = overflow_animation_progress_;
    overflow_animation_target_ = 0.0;
    overflow_animation_start_time_us_ = 0;
    overflow_animation_active_ = true;
    ensure_animation_tick();
    if (canvas_ != nullptr) gtk_widget_queue_draw(canvas_);
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

void WorkspaceOverviewOverlay::set_morph_sources(
    std::vector<animation::WorkspaceMorphSource> sources
) {
    if (morph_timeline_.active()) return;
    morph_sources_ = std::move(sources);
}

void WorkspaceOverviewOverlay::set_morph_input_enabled(bool enabled) noexcept {
    if (canvas_ == nullptr) return;
    gtk_widget_set_can_target(canvas_, enabled);
}

bool WorkspaceOverviewOverlay::morph_interactive() const noexcept {
    return morph_timeline_.state() == effects::TransitionState::Visible;
}

animation::WorkspaceMorphLayout WorkspaceOverviewOverlay::morph_layout(
    double scale_x,
    double scale_y
) const noexcept {
    std::array<int, animation::kWorkspaceMorphBandCount> workspace_ids{};
    std::array<double, animation::kWorkspaceMorphBandCount>
        destination_heights{};
    if (morph_geometry_frozen_) {
        workspace_ids = morph_workspace_ids_;
        destination_heights = morph_destination_heights_;
    } else {
        for (std::size_t index = 0; index < workspace_ids.size(); ++index) {
            workspace_ids[index] = workspace_state_[index].workspace_id;
        }
        destination_heights = displayed_heights_;
    }

    return animation::build_workspace_morph_layout(
        workspace_ids,
        destination_heights,
        animation::scale_workspace_morph_sources_to_reference(
            morph_sources_,
            scale_x,
            scale_y
        ),
        kReferenceWidth,
        kReferenceHeight
    );
}

void WorkspaceOverviewOverlay::capture_morph_geometry() noexcept {
    for (std::size_t index = 0; index < morph_workspace_ids_.size(); ++index) {
        morph_workspace_ids_[index] = workspace_state_[index].workspace_id;
    }
    morph_destination_heights_ = displayed_heights_;
    morph_geometry_frozen_ = true;
}

void WorkspaceOverviewOverlay::schedule_morph_shader_capture() noexcept {
    if (morph_renderer_.active()) return;
    morph_shader_capture_pending_ = true;
    morph_shader_failed_for_transition_ = false;
}

void WorkspaceOverviewOverlay::try_begin_morph_shader() noexcept {
    if (!morph_shader_capture_pending_ || morph_renderer_.active() ||
        morph_shader_failed_for_transition_ || canvas_ == nullptr ||
        overlay_stack_ == nullptr) {
        return;
    }

    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    GtkNative* native = gtk_widget_get_native(overlay_stack_);
    if (width <= 0 || height <= 0 || native == nullptr ||
        gtk_native_get_renderer(native) == nullptr) {
        return;
    }

    const double scale_x = static_cast<double>(width) / kReferenceWidth;
    const double scale_y = static_cast<double>(height) / kReferenceHeight;
    const auto layout = morph_layout(scale_x, scale_y);
    const auto geometry = animation::build_workspace_morph_shader_geometry(
        layout
    );

    force_native_capture_ = true;
    std::string error;
    const bool started = morph_renderer_.begin(
        overlay_stack_,
        canvas_,
        morph_timeline_.target_visible(),
        geometry,
        {},
        &error
    );
    force_native_capture_ = false;
    morph_shader_capture_pending_ = false;

    if (!started) {
        morph_shader_failed_for_transition_ = true;
        morph_diagnostics_.note_shader_failed();
        std::cerr << "[Realmheart workspace morph] Geometry fallback: "
                  << error << '\n';
        return;
    }
    morph_diagnostics_.note_shader_started(
        morph_renderer_.transient_source_bytes()
    );
    update_morph_shader();
}

void WorkspaceOverviewOverlay::update_morph_shader() noexcept {
    if (morph_renderer_.failed()) {
        morph_diagnostics_.note_shader_failed();
        return;
    }
    if (!morph_renderer_.active() || canvas_ == nullptr) return;
    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    if (width <= 0 || height <= 0) return;

    const double scale_x = static_cast<double>(width) / kReferenceWidth;
    const double scale_y = static_cast<double>(height) / kReferenceHeight;
    const auto layout = morph_layout(scale_x, scale_y);
    const auto frame = animation::sample_workspace_morph_frame(
        layout,
        morph_timeline_.progress()
    );
    morph_renderer_.update(
        morph_timeline_.progress(),
        morph_timeline_.target_visible(),
        animation::build_workspace_morph_shader_frame(layout, frame)
    );
    morph_diagnostics_.note_transient_bytes(
        morph_renderer_.transient_source_bytes()
    );
    if (morph_renderer_.frame_ready()) {
        morph_diagnostics_.note_shader_ready(
            morph_renderer_.transient_source_bytes()
        );
    }
}

void WorkspaceOverviewOverlay::finish_morph_endpoint() noexcept {
    morph_last_frame_time_us_ = 0;
    morph_shader_capture_pending_ = false;
    morph_shader_failed_for_transition_ = false;
    force_native_capture_ = false;
    morph_diagnostics_.note_transient_bytes(
        morph_renderer_.transient_source_bytes()
    );
    if (morph_renderer_.failed()) {
        morph_diagnostics_.note_shader_failed();
    }
    morph_renderer_.finish();
    if (morph_diagnostics_.active()) {
        const auto diagnostics = morph_diagnostics_.finish(
            morph_timeline_.state() == effects::TransitionState::Visible,
            monotonic_seconds(),
            animation::workspace_morph_process_rss_kib(),
            morph_renderer_.transient_source_bytes()
        );
        std::cerr << "[Realmheart workspace morph] "
                  << animation::format_workspace_morph_diagnostics(diagnostics)
                  << '\n';
    }
    morph_geometry_frozen_ = false;
    if (set_taskbar_morph_progress_) {
        set_taskbar_morph_progress_(morph_timeline_.progress());
    }
    if (morph_timeline_.state() == effects::TransitionState::Visible) {
        set_morph_input_enabled(true);
        if (canvas_ != nullptr) gtk_widget_grab_focus(canvas_);
        return;
    }
    if (morph_timeline_.state() != effects::TransitionState::Hidden) return;

    set_morph_input_enabled(false);
    if (window_ != nullptr) {
        gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    }
    if (taskbar_morph_active_) {
        if (set_taskbar_morph_active_) set_taskbar_morph_active_(false);
        taskbar_morph_active_ = false;
    }
}

void WorkspaceOverviewOverlay::prepare_card_transition(
    const WorkspaceOverviewState& next
) {
    finish_card_transition();
    if (!morph_interactive() || !assets_attempted_ || !asset_error_.empty()) {
        return;
    }

    bool has_motion = false;
    for (std::size_t realm = 0; realm < workspace_state_.size(); ++realm) {
        const auto& previous_realm = workspace_state_[realm];
        const auto& next_realm = next[realm];

        const bool same_workspace = previous_realm.workspace_id ==
            next_realm.workspace_id;
        for (std::size_t slot = 0; slot < next_realm.card_count; ++slot) {
            const auto previous_slot = same_workspace
                ? find_card_slot(previous_realm, next_realm.cards[slot])
                : std::nullopt;
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
        const std::size_t previous_style = style_index_for_workspace_id(
            previous_realm.workspace_id
        );
        for (std::size_t slot = 0; slot < previous_realm.card_count; ++slot) {
            if (same_workspace &&
                find_card_slot(next_realm, previous_realm.cards[slot]).has_value()) {
                continue;
            }
            if (outgoing_index >= outgoing_cards_[realm].size()) break;

            auto& outgoing = outgoing_cards_[realm][outgoing_index++];
            const auto& previous_assets = assets_[previous_style].cards[slot];
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

std::size_t WorkspaceOverviewOverlay::style_index_for_realm(
    std::size_t realm_index
) const noexcept {
    if (realm_index >= workspace_state_.size()) {
        return realm_index % kWorkspaceOverviewRealmCount;
    }
    return style_index_for_workspace_id(
        workspace_state_[realm_index].workspace_id
    );
}

double WorkspaceOverviewOverlay::viewport_visual_offset() const noexcept {
    if (!viewport_animation_active_) return 0.0;
    return static_cast<double>(viewport_transition_direction_) *
        kViewportTransitionDistance * (1.0 - viewport_animation_progress_);
}

void WorkspaceOverviewOverlay::begin_viewport_transition(int direction) {
    if (!morph_interactive() || direction == 0) {
        viewport_transition_direction_ = 0;
        viewport_animation_start_time_us_ = 0;
        viewport_animation_progress_ = 1.0;
        viewport_animation_active_ = false;
        return;
    }
    viewport_transition_direction_ = direction > 0 ? 1 : -1;
    viewport_animation_start_time_us_ = 0;
    viewport_animation_progress_ = 0.0;
    viewport_animation_active_ = true;
    ensure_animation_tick();
}

void WorkspaceOverviewOverlay::synchronize_active_workspace() {
    for (std::size_t index = 0; index < workspace_state_.size(); ++index) {
        if (!workspace_state_[index].active) continue;
        active_index_ = static_cast<int>(index);
        stop_content_animations(false);
        displayed_heights_ = target_realm_heights(active_index_);
        animation_start_heights_ = displayed_heights_;
        animation_target_heights_ = displayed_heights_;
        normalize_selection();
        return;
    }
}

cairo_surface_t* WorkspaceOverviewOverlay::application_icon_surface(
    std::string_view requested_icon_name,
    std::string_view app_name
) {
    const std::string cache_key = std::string(requested_icon_name) + '\n' +
        std::string(app_name);
    const auto cached = icon_surfaces_.find(cache_key);
    if (cached != icon_surfaces_.end()) return cached->second;

    auto candidates = basic_icon_candidates(requested_icon_name);
    std::unordered_set<std::string> seen(
        candidates.begin(),
        candidates.end()
    );
    const std::string requested_identity = normalized_app_identifier(
        requested_icon_name
    );
    const std::string app_identity = normalized_app_identifier(app_name);

    GList* applications = g_app_info_get_all();
    for (GList* item = applications; item != nullptr; item = item->next) {
        GAppInfo* info = G_APP_INFO(item->data);
        const char* id = g_app_info_get_id(info);
        const char* executable = g_app_info_get_executable(info);
        const char* display_name = g_app_info_get_display_name(info);
        const bool identifier_match = id != nullptr &&
            normalized_app_identifier(id) == requested_identity;
        const bool executable_match = executable != nullptr &&
            normalized_app_identifier(executable) == requested_identity;
        const bool display_match = display_name != nullptr &&
            normalized_app_identifier(display_name) == app_identity;
        if (!identifier_match && !executable_match && !display_match) continue;

        GIcon* icon = g_app_info_get_icon(info);
        if (icon == nullptr || !G_IS_THEMED_ICON(icon)) continue;
        const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
        if (names == nullptr) continue;
        for (std::size_t index = 0; names[index] != nullptr; ++index) {
            append_unique_icon_candidate(candidates, seen, names[index]);
        }
    }
    g_list_free_full(applications, g_object_unref);
    append_unique_icon_candidate(
        candidates,
        seen,
        "application-x-executable"
    );

    GdkDisplay* display = window_ != nullptr
        ? gtk_widget_get_display(GTK_WIDGET(window_))
        : nullptr;
    GtkIconTheme* theme = display != nullptr
        ? gtk_icon_theme_get_for_display(display)
        : nullptr;
    if (theme == nullptr) {
        icon_surfaces_.emplace(cache_key, nullptr);
        return nullptr;
    }

    const int scale = std::max(
        1,
        canvas_ != nullptr ? gtk_widget_get_scale_factor(canvas_) : 1
    );
    for (const auto& candidate : candidates) {
        if (!gtk_icon_theme_has_icon(theme, candidate.c_str())) continue;
        GtkIconPaintable* paintable = gtk_icon_theme_lookup_icon(
            theme,
            candidate.c_str(),
            nullptr,
            96,
            scale,
            GTK_TEXT_DIR_NONE,
            GTK_ICON_LOOKUP_FORCE_REGULAR
        );
        if (paintable == nullptr) continue;

        GFile* file = gtk_icon_paintable_get_file(paintable);
        g_object_unref(paintable);
        if (file == nullptr) continue;

        GError* error = nullptr;
        GdkTexture* texture = gdk_texture_new_from_file(file, &error);
        g_object_unref(file);
        if (texture == nullptr) {
            g_clear_error(&error);
            continue;
        }

        const int width = gdk_texture_get_width(texture);
        const int height = gdk_texture_get_height(texture);
        cairo_surface_t* surface = width > 0 && height > 0
            ? cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height)
            : nullptr;
        if (surface == nullptr ||
            cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            if (surface != nullptr) cairo_surface_destroy(surface);
            g_object_unref(texture);
            continue;
        }
        gdk_texture_download(
            texture,
            cairo_image_surface_get_data(surface),
            static_cast<gsize>(cairo_image_surface_get_stride(surface))
        );
        cairo_surface_mark_dirty(surface);
        g_object_unref(texture);
        icon_surfaces_.emplace(cache_key, surface);
        return surface;
    }

    icon_surfaces_.emplace(cache_key, nullptr);
    return nullptr;
}

void WorkspaceOverviewOverlay::clear_icon_cache() noexcept {
    for (const auto& [key, surface] : icon_surfaces_) {
        static_cast<void>(key);
        if (surface != nullptr) cairo_surface_destroy(surface);
    }
    icon_surfaces_.clear();
}

bool WorkspaceOverviewOverlay::rebuild_dirty_overlays() {
    bool success = true;
    for (std::size_t index = 0; index < workspace_state_.size(); ++index) {
        if (!overlay_dirty_[index]) continue;

        const std::size_t style_index = style_index_for_realm(index);
        auto& style_assets = assets_[style_index];
        const auto& realm_state = workspace_state_[index];
        if (style_assets.background == nullptr ||
            style_assets.character == nullptr ||
            style_assets.element_layout == nullptr ||
            style_assets.place_layout == nullptr) {
            success = false;
            continue;
        }

        std::string render_error;
        if (style_assets.roman_layout == nullptr ||
            style_assets.roman_workspace_id != realm_state.workspace_id) {
            g_clear_object(&style_assets.roman_layout);
            const std::string roman = workspace_roman_numeral(
                realm_state.workspace_id
            );
            style_assets.roman_layout = create_identity_layout(
                canvas_,
                roman,
                60.0,
                "Georgia",
                0,
                render_error
            );
            style_assets.roman_workspace_id =
                style_assets.roman_layout != nullptr
                ? realm_state.workspace_id
                : 0;
        }

        GdkTexture* compact = style_assets.compact_overlay != nullptr
            ? GDK_TEXTURE(g_object_ref(style_assets.compact_overlay))
            : render_realm_overlay_texture(
                style_index,
                realm_state,
                false,
                render_error
            );
        GdkTexture* expanded = style_assets.expanded_overlay != nullptr
            ? GDK_TEXTURE(g_object_ref(style_assets.expanded_overlay))
            : compact != nullptr
                ? render_realm_overlay_texture(
                    style_index,
                    realm_state,
                    true,
                    render_error
                )
                : nullptr;
        std::array<CardAssets, kWorkspaceOverviewCardLimit> cards{};
        std::vector<GdkTexture*> spread_cards;
        bool cards_ready = style_assets.roman_layout != nullptr &&
            compact != nullptr && expanded != nullptr;
        for (std::size_t slot = 0;
             cards_ready && slot < realm_state.card_count;
             ++slot) {
            cards[slot].compact = render_card_texture(
                style_index,
                realm_state.cards[slot],
                application_icon_surface(
                    realm_state.cards[slot].icon_name,
                    realm_state.cards[slot].app_name
                ),
                slot,
                false,
                render_error
            );
            cards[slot].expanded = cards[slot].compact != nullptr
                ? render_card_texture(
                    style_index,
                    realm_state.cards[slot],
                    application_icon_surface(
                        realm_state.cards[slot].icon_name,
                        realm_state.cards[slot].app_name
                    ),
                    slot,
                    true,
                    render_error
                )
                : nullptr;
            cards_ready = cards[slot].compact != nullptr &&
                cards[slot].expanded != nullptr;
        }

        if (cards_ready && realm_state.windows.size() >
                kWorkspaceOverviewPreviewCardLimit) {
            const auto target_rects = overflow_card_rects(
                0.0,
                kActiveFraction * kReferenceHeight,
                realm_state.windows.size()
            );
            spread_cards.reserve(realm_state.windows.size());
            for (std::size_t slot = 0;
                 cards_ready && slot < realm_state.windows.size();
                 ++slot) {
                const auto& window = realm_state.windows[slot];
                const auto& rect = target_rects[slot];
                GdkTexture* texture = render_overflow_card_texture(
                    style_index,
                    window,
                    application_icon_surface(window.icon_name, window.app_name),
                    rect.width,
                    rect.height,
                    render_error
                );
                spread_cards.push_back(texture);
                cards_ready = texture != nullptr;
            }
        }

        if (!cards_ready) {
            g_clear_object(&compact);
            g_clear_object(&expanded);
            for (auto& card : cards) {
                g_clear_object(&card.compact);
                g_clear_object(&card.expanded);
            }
            for (auto*& card : spread_cards) g_clear_object(&card);
            std::cerr
                << "[WorkspaceOverview] unable to refresh workspace "
                << realm_state.workspace_id << " cards";
            if (!render_error.empty()) std::cerr << ": " << render_error;
            std::cerr << '\n';
            success = false;
            continue;
        }

        g_clear_object(&style_assets.compact_overlay);
        g_clear_object(&style_assets.expanded_overlay);
        for (auto& card : style_assets.cards) {
            g_clear_object(&card.compact);
            g_clear_object(&card.expanded);
        }
        for (auto*& card : style_assets.spread_cards) g_clear_object(&card);
        style_assets.spread_cards.clear();
        style_assets.compact_overlay = compact;
        style_assets.expanded_overlay = expanded;
        style_assets.cards = cards;
        style_assets.spread_cards = std::move(spread_cards);
        overlay_dirty_[index] = false;
    }
    return success;
}

void WorkspaceOverviewOverlay::set_workspace_snapshot(
    const services::WorkspaceSnapshot& snapshot
) {
    int previous_active_workspace_id = 0;
    for (const auto& realm : workspace_state_) {
        if (realm.active) {
            previous_active_workspace_id = realm.workspace_id;
            break;
        }
    }
    const int next_viewport_start = visible_workspace_start_for_active(
        snapshot.active_id
    );
    const int viewport_direction = next_viewport_start ==
            viewport_start_workspace_id_
        ? 0
        : next_viewport_start > viewport_start_workspace_id_ ? 1 : -1;
    auto next = build_workspace_overview_state(
        snapshot,
        next_viewport_start
    );

    bool cards_changed = false;
    for (std::size_t index = 0; index < next.size(); ++index) {
        if (!same_workspace_overview_cards(workspace_state_[index], next[index])) {
            overlay_dirty_[index] = true;
            cards_changed = true;
        }
    }
    if (cards_changed) prepare_card_transition(next);

    workspace_state_ = std::move(next);
    viewport_start_workspace_id_ = next_viewport_start;

    if (overflow_workspace_id_ != 0) {
        const auto spread_realm = std::find_if(
            workspace_state_.begin(),
            workspace_state_.end(),
            [this](const WorkspaceOverviewRealm& realm) {
                return realm.workspace_id == overflow_workspace_id_;
            }
        );
        if (spread_realm == workspace_state_.end() ||
            spread_realm->windows.size() <= kWorkspaceOverviewPreviewCardLimit) {
            collapse_overflow(true);
        }
    }

    if (!morph_interactive()) {
        synchronize_active_workspace();
    } else {
        const bool active_workspace_changed = snapshot.active_id !=
            previous_active_workspace_id;
        if (active_workspace_changed || viewport_direction != 0) {
            for (std::size_t index = 0;
                 index < workspace_state_.size();
                 ++index) {
                if (!workspace_state_[index].active) continue;
                select_realm(static_cast<int>(index), 0);
                break;
            }
        }
        if (viewport_direction != 0) {
            begin_viewport_transition(viewport_direction);
        }
    }
    normalize_selection();

    if (visible() && assets_attempted_ && asset_error_.empty()) {
        static_cast<void>(rebuild_dirty_overlays());
    }

    if (drag_card_.active) {
        const auto target = realm_index_at_point(
            drag_card_.current_x,
            drag_card_.current_y - viewport_visual_offset(),
            displayed_heights_
        );
        drag_target_index_ = target.has_value() ? *target : -1;
    }
    if (canvas_ != nullptr) gtk_widget_queue_draw(canvas_);
}

void WorkspaceOverviewOverlay::show() {
    const auto state = morph_timeline_.state();
    if (state == effects::TransitionState::Visible ||
        state == effects::TransitionState::Opening) {
        return;
    }

    if (state == effects::TransitionState::Hidden) {
        if (morph_diagnostics_.enabled()) {
            morph_diagnostics_.begin(
                true,
                monotonic_seconds(),
                animation::workspace_morph_process_rss_kib()
            );
        }
        synchronize_active_workspace();
        normalize_selection();
        static_cast<void>(ensure_assets());
        static_cast<void>(rebuild_dirty_overlays());
        stop_content_animations(true);
        capture_morph_geometry();
        if (!taskbar_morph_active_) {
            if (set_taskbar_morph_active_) set_taskbar_morph_active_(true);
            taskbar_morph_active_ = true;
        }
        if (set_taskbar_morph_progress_) {
            set_taskbar_morph_progress_(morph_timeline_.progress());
        }
        gtk_window_present(window_);
        schedule_morph_shader_capture();
    }

    morph_diagnostics_.set_direction(true);
    set_morph_input_enabled(false);
    morph_timeline_.open();
    morph_last_frame_time_us_ = 0;
    ensure_animation_tick();
    gtk_widget_queue_draw(canvas_);
    gtk_widget_grab_focus(canvas_);
    if (!morph_timeline_.active()) finish_morph_endpoint();
}

void WorkspaceOverviewOverlay::hide() {
    if (morph_timeline_.state() == effects::TransitionState::Hidden ||
        morph_timeline_.state() == effects::TransitionState::Closing) {
        return;
    }
    if (morph_timeline_.state() == effects::TransitionState::Visible &&
        morph_diagnostics_.enabled()) {
        morph_diagnostics_.begin(
            false,
            monotonic_seconds(),
            animation::workspace_morph_process_rss_kib()
        );
    }
    morph_diagnostics_.set_direction(false);
    reset_drag();
    collapse_overflow(true);
    stop_content_animations(true);
    if (morph_timeline_.state() == effects::TransitionState::Visible) {
        capture_morph_geometry();
        schedule_morph_shader_capture();
    }
    set_morph_input_enabled(false);
    morph_timeline_.close();
    morph_last_frame_time_us_ = 0;
    ensure_animation_tick();
    if (canvas_ != nullptr) gtk_widget_queue_draw(canvas_);
    if (!morph_timeline_.active()) finish_morph_endpoint();
}

void WorkspaceOverviewOverlay::toggle() {
    if (morph_timeline_.target_visible()) hide(); else show();
}

void WorkspaceOverviewOverlay::prewarm() {
    if (prewarmed_) return;
    if (window_ == nullptr || canvas_ == nullptr) return;
    if (morph_timeline_.target_visible()) {
        // Real show() already does this work visibly; mark warm.
        prewarmed_ = true;
        return;
    }
    prewarmed_ = true;

    // CPU-side first-open work: asset decode + overlay rasterization.
    synchronize_active_workspace();
    normalize_selection();
    static_cast<void>(ensure_assets());
    static_cast<void>(rebuild_dirty_overlays());

    // GPU-side first-open work: map once at opacity 0 and let exactly one
    // frame clock tick produce a real frame through the full GSK/EGL
    // pipeline. The window stays invisible the whole time, so there is no
    // perceptible flash even on a fast display.
    gtk_widget_set_opacity(GTK_WIDGET(window_), 0.0);
    gtk_window_present(window_);
    gtk_widget_queue_draw(canvas_);

    struct PrewarmFrameState {
        guint tick_id = 0;
        WorkspaceOverviewOverlay* self = nullptr;
    };
    auto* state = new PrewarmFrameState{0, this};
    state->tick_id = gtk_widget_add_tick_callback(
        canvas_,
        +[](
            GtkWidget*,
            GdkFrameClock*,
            gpointer data
        ) -> gboolean {
            auto* frame_state = static_cast<PrewarmFrameState*>(data);
            auto* overlay = frame_state->self;
            gtk_widget_remove_tick_callback(
                GTK_WIDGET(overlay->canvas_),
                frame_state->tick_id
            );
            delete frame_state;
            // Only unmap when still in the hidden prewarm state — a real
            // toggle that raced us owns the window from here on. Restoring
            // opacity is safe either way: show()/hide() paths set their own
            // visual state every transition.
            const bool still_hidden =
                !overlay->morph_timeline_.target_visible();
            if (still_hidden) {
                gtk_widget_set_visible(GTK_WIDGET(overlay->window_), FALSE);
            }
            gtk_widget_set_opacity(GTK_WIDGET(overlay->window_), 1.0);
            return G_SOURCE_REMOVE;
        },
        state,
        nullptr
    );
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

    self->try_begin_morph_shader();
    if (self->morph_timeline_.active()) {
        if (self->morph_last_frame_time_us_ == 0) {
            self->morph_last_frame_time_us_ = frame_time_us;
            keep_running = true;
        } else {
            const double elapsed_seconds = static_cast<double>(
                frame_time_us - self->morph_last_frame_time_us_
            ) / static_cast<double>(kMicrosecondsPerSecond);
            self->morph_last_frame_time_us_ = frame_time_us;
            self->morph_diagnostics_.record_frame(elapsed_seconds);
            if (self->morph_timeline_.advance(elapsed_seconds)) {
                keep_running = true;
            } else {
                self->finish_morph_endpoint();
            }
        }
    }
    if (self->set_taskbar_morph_progress_ && self->taskbar_morph_active_) {
        self->set_taskbar_morph_progress_(self->morph_timeline_.progress());
    }
    self->update_morph_shader();

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

    if (self->overflow_animation_active_) {
        if (self->overflow_animation_start_time_us_ == 0) {
            self->overflow_animation_start_time_us_ = frame_time_us;
        }
        const double elapsed_seconds = static_cast<double>(
            frame_time_us - self->overflow_animation_start_time_us_
        ) / static_cast<double>(kMicrosecondsPerSecond);
        const double distance = std::max(
            0.001,
            std::abs(
                self->overflow_animation_target_ -
                self->overflow_animation_start_progress_
            )
        );
        const double linear = std::clamp(
            elapsed_seconds /
                (kOverflowTransitionDurationSeconds * distance),
            0.0,
            1.0
        );
        self->overflow_animation_progress_ = lerp(
            self->overflow_animation_start_progress_,
            self->overflow_animation_target_,
            transition_ease(linear)
        );
        if (linear >= 1.0) {
            self->overflow_animation_progress_ =
                self->overflow_animation_target_;
            self->overflow_animation_start_time_us_ = 0;
            self->overflow_animation_active_ = false;
            if (self->overflow_animation_target_ <= 0.001) {
                self->overflow_workspace_id_ = 0;
                self->normalize_selection();
            }
        } else {
            keep_running = true;
        }
    }

    if (self->selection_animation_active_) {
        if (self->selection_animation_start_time_us_ == 0) {
            self->selection_animation_start_time_us_ = frame_time_us;
        }
        const double elapsed_seconds = static_cast<double>(
            frame_time_us - self->selection_animation_start_time_us_
        ) / static_cast<double>(kMicrosecondsPerSecond);
        const double linear = std::clamp(
            elapsed_seconds / kSelectionTransitionDurationSeconds,
            0.0,
            1.0
        );
        self->selection_animation_progress_ = transition_ease(linear);
        if (linear >= 1.0) {
            self->finish_selection_transition();
        } else {
            keep_running = true;
        }
    }

    if (self->viewport_animation_active_) {
        if (self->viewport_animation_start_time_us_ == 0) {
            self->viewport_animation_start_time_us_ = frame_time_us;
        }
        const double elapsed_seconds = static_cast<double>(
            frame_time_us - self->viewport_animation_start_time_us_
        ) / static_cast<double>(kMicrosecondsPerSecond);
        const double linear = std::clamp(
            elapsed_seconds / kViewportTransitionDurationSeconds,
            0.0,
            1.0
        );
        self->viewport_animation_progress_ = transition_ease(linear);
        if (linear >= 1.0) {
            self->viewport_animation_start_time_us_ = 0;
            self->viewport_animation_progress_ = 1.0;
            self->viewport_animation_active_ = false;
            self->viewport_transition_direction_ = 0;
        } else {
            keep_running = true;
        }
    }

    gtk_widget_queue_draw(self->canvas_);
    if (!keep_running && !self->realm_animation_active_ &&
        !self->card_animation_active_ &&
        !self->overflow_animation_active_ &&
        !self->selection_animation_active_ &&
        !self->viewport_animation_active_ &&
        !self->morph_timeline_.active()) {
        self->animation_tick_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

bool WorkspaceOverviewOverlay::handle_key_pressed(guint keyval) {
    if (keyval == GDK_KEY_Escape) {
        if (overflow_workspace_id_ != 0) {
            collapse_overflow(false);
        } else {
            hide();
        }
        return true;
    }
    if (!morph_interactive()) return true;
    if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_4) {
        select_realm(static_cast<int>(keyval - GDK_KEY_1));
        return true;
    }

    switch (keyval) {
        case GDK_KEY_Up:
        case GDK_KEY_k:
        case GDK_KEY_K:
            move_realm_selection(-1);
            return true;
        case GDK_KEY_Down:
        case GDK_KEY_j:
        case GDK_KEY_J:
            move_realm_selection(1);
            return true;
        case GDK_KEY_Left:
        case GDK_KEY_h:
        case GDK_KEY_H:
            move_card_selection(-1);
            return true;
        case GDK_KEY_Right:
        case GDK_KEY_l:
        case GDK_KEY_L:
            move_card_selection(1);
            return true;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_ISO_Enter:
            activate_selected();
            return true;
        default:
            return false;
    }
}

void WorkspaceOverviewOverlay::move_realm_selection(int delta) {
    const int count = static_cast<int>(workspace_state_.size());
    if (count <= 0 || delta == 0) return;
    const int next = (active_index_ + delta % count + count) % count;
    select_realm(next);
}

void WorkspaceOverviewOverlay::move_card_selection(int delta) {
    if (active_index_ < 0 ||
        active_index_ >= static_cast<int>(workspace_state_.size()) ||
        delta == 0) {
        return;
    }

    const std::size_t realm_index = static_cast<std::size_t>(active_index_);
    const auto& realm = workspace_state_[realm_index];
    const int count = static_cast<int>(
        overflow_expanded_for(realm_index)
            ? realm.windows.size()
            : realm.card_count
    );
    int next_card = -1;
    if (count > 0 && selected_card_index_ < 0) {
        next_card = delta > 0 ? 0 : count - 1;
    } else if (count > 0) {
        next_card = (selected_card_index_ + delta % count + count) % count;
    }
    transition_selection(active_index_, next_card);
}

void WorkspaceOverviewOverlay::activate_selected() {
    if (active_index_ < 0 ||
        active_index_ >= static_cast<int>(workspace_state_.size())) {
        return;
    }

    const std::size_t realm_index = static_cast<std::size_t>(active_index_);
    const auto& realm = workspace_state_[realm_index];
    if (overflow_expanded_for(realm_index)) {
        if (selected_card_index_ >= 0 &&
            selected_card_index_ < static_cast<int>(realm.windows.size())) {
            const auto& card = realm.windows[
                static_cast<std::size_t>(selected_card_index_)
            ];
            if (!card.address.empty() && activate_window_) {
                activate_window_(realm.workspace_id, card.address);
                hide();
            }
        }
        return;
    }

    if (selected_card_index_ >= 0 &&
        selected_card_index_ < static_cast<int>(realm.card_count)) {
        const auto& card = realm.cards[
            static_cast<std::size_t>(selected_card_index_)
        ];
        if (card.summary) {
            expand_overflow(realm_index);
            return;
        }
        if (!card.address.empty() && activate_window_) {
            activate_window_(realm.workspace_id, card.address);
        } else if (activate_workspace_) {
            activate_workspace_(realm.workspace_id);
        }
    } else if (activate_workspace_) {
        activate_workspace_(realm.workspace_id);
    }
    hide();
}

void WorkspaceOverviewOverlay::normalize_selection() noexcept {
    if (active_index_ < 0 ||
        active_index_ >= static_cast<int>(workspace_state_.size())) {
        selected_card_index_ = -1;
        return;
    }
    const std::size_t realm_index = static_cast<std::size_t>(active_index_);
    const auto& realm = workspace_state_[realm_index];
    const int count = static_cast<int>(
        overflow_expanded_for(realm_index)
            ? realm.windows.size()
            : realm.card_count
    );
    if (count <= 0) {
        selected_card_index_ = -1;
    } else {
        selected_card_index_ = std::clamp(selected_card_index_, 0, count - 1);
    }
}

void WorkspaceOverviewOverlay::select_realm(
    int index,
    int preferred_card_index
) {
    if (index < 0 || index >= static_cast<int>(displayed_heights_.size())) return;

    if (index != active_index_ && overflow_workspace_id_ != 0) {
        collapse_overflow(false);
    }
    const std::size_t realm_index = static_cast<std::size_t>(index);
    const auto& realm = workspace_state_[realm_index];
    const int card_count = static_cast<int>(
        overflow_expanded_for(realm_index)
            ? realm.windows.size()
            : realm.card_count
    );
    int next_card = card_count > 0 ? 0 : -1;
    if (preferred_card_index != kDefaultCardSelection) {
        next_card = card_count > 0
            ? std::clamp(preferred_card_index, 0, card_count - 1)
            : -1;
    }

    // Motion events arrive continuously while the pointer moves inside a realm.
    // Do not restart an in-flight realm transition that is already targeting it.
    if (index == active_index_) {
        if (preferred_card_index == kDefaultCardSelection) {
            normalize_selection();
        } else {
            transition_selection(index, next_card);
        }
        return;
    }

    transition_selection(index, next_card);
    const auto target = target_realm_heights(index);
    animation_start_heights_ = displayed_heights_;
    animation_target_heights_ = target;
    animation_start_time_us_ = 0;
    realm_animation_active_ = true;
    ensure_animation_tick();
}

std::optional<graphene_rect_t>
WorkspaceOverviewOverlay::selected_card_outline_bounds() const {
    if (active_index_ < 0 ||
        active_index_ >= static_cast<int>(workspace_state_.size()) ||
        selected_card_index_ < 0) {
        return std::nullopt;
    }

    const std::size_t realm_index = static_cast<std::size_t>(active_index_);
    const std::size_t slot = static_cast<std::size_t>(selected_card_index_);
    const auto tops = realm_tops(displayed_heights_);
    const double realm_height = displayed_heights_[realm_index];
    const double activity = realm_activity(realm_height);
    if (overflow_expanded_for(realm_index)) {
        const auto& realm = workspace_state_[realm_index];
        if (slot >= realm.windows.size()) return std::nullopt;
        return rect_to_graphene(overflow_card_current_rect(
            slot,
            realm.windows.size(),
            tops[realm_index],
            realm_height,
            activity,
            overflow_animation_progress_
        ));
    }
    if (slot >= workspace_state_[realm_index].card_count ||
        slot >= kWorkspaceOverviewCardLimit) {
        return std::nullopt;
    }

    const double card_progress = card_animation_active_
        ? card_animation_progress_
        : 1.0;
    const int configured_from = card_from_slots_[realm_index][slot];
    const std::size_t from_slot = configured_from >= 0
        ? static_cast<std::size_t>(configured_from)
        : slot;
    const bool entering = card_animation_active_ &&
        card_entering_[realm_index][slot];

    const Rect compact_target = scaled_card_rect(
        slot,
        false,
        tops[realm_index],
        realm_height
    );
    const Rect expanded_target = scaled_card_rect(
        slot,
        true,
        tops[realm_index],
        realm_height
    );
    const Rect compact_start = entering
        ? translated_rect(compact_target, -kCardTransitionDistance)
        : scaled_card_rect(
            from_slot,
            false,
            tops[realm_index],
            realm_height
        );
    const Rect expanded_start = entering
        ? translated_rect(expanded_target, -kCardTransitionDistance)
        : scaled_card_rect(
            from_slot,
            true,
            tops[realm_index],
            realm_height
        );
    const Rect compact_current = lerp_rect(
        compact_start,
        compact_target,
        card_progress
    );
    const Rect expanded_current = lerp_rect(
        expanded_start,
        expanded_target,
        card_progress
    );
    return rect_to_graphene(lerp_rect(
        compact_current,
        expanded_current,
        activity
    ));
}

void WorkspaceOverviewOverlay::transition_selection(
    int realm_index,
    int card_index
) {
    if (realm_index == active_index_ && card_index == selected_card_index_) return;

    const auto old_target = selected_card_outline_bounds();
    std::optional<graphene_rect_t> current_bounds;
    double current_opacity = old_target.has_value() ? 1.0 : 0.0;
    if (selection_animation_active_) {
        if (selection_outline_has_start_bounds_) {
            current_bounds = old_target.has_value()
                ? lerp_graphene_rect(
                    selection_outline_start_bounds_,
                    *old_target,
                    selection_animation_progress_
                )
                : selection_outline_start_bounds_;
        } else if (old_target.has_value()) {
            current_bounds = *old_target;
        }
        current_opacity = lerp(
            selection_outline_start_opacity_,
            old_target.has_value() ? 1.0 : 0.0,
            selection_animation_progress_
        );
    } else if (old_target.has_value()) {
        current_bounds = *old_target;
    }

    active_index_ = realm_index;
    selected_card_index_ = card_index;
    const auto next_target = selected_card_outline_bounds();

    if (current_bounds.has_value()) {
        selection_outline_start_bounds_ = *current_bounds;
        selection_outline_has_start_bounds_ = true;
    } else if (next_target.has_value()) {
        selection_outline_start_bounds_ = *next_target;
        selection_outline_has_start_bounds_ = true;
    } else {
        finish_selection_transition();
        if (canvas_ != nullptr) gtk_widget_queue_draw(canvas_);
        return;
    }

    selection_outline_start_opacity_ = current_opacity;
    selection_animation_progress_ = 0.0;
    selection_animation_start_time_us_ = 0;
    selection_animation_active_ = true;
    ensure_animation_tick();
}

void WorkspaceOverviewOverlay::finish_selection_transition() noexcept {
    selection_animation_start_time_us_ = 0;
    selection_animation_progress_ = 1.0;
    selection_animation_active_ = false;
    selection_outline_has_start_bounds_ = false;
    selection_outline_start_opacity_ = 0.0;
}

void WorkspaceOverviewOverlay::stop_content_animations(
    bool snap_to_target
) noexcept {
    animation_start_time_us_ = 0;
    realm_animation_active_ = false;
    finish_card_transition();
    finish_selection_transition();
    viewport_animation_start_time_us_ = 0;
    viewport_animation_progress_ = 1.0;
    viewport_animation_active_ = false;
    viewport_transition_direction_ = 0;
    if (snap_to_target) {
        displayed_heights_ = animation_target_heights_;
        animation_start_heights_ = animation_target_heights_;
    }
}

void WorkspaceOverviewOverlay::stop_animation(bool snap_to_target) noexcept {
    if (animation_tick_id_ != 0 && canvas_ != nullptr) {
        gtk_widget_remove_tick_callback(canvas_, animation_tick_id_);
        animation_tick_id_ = 0;
    }
    stop_content_animations(snap_to_target);
    morph_last_frame_time_us_ = 0;
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
    const double morph_progress = force_native_capture_
        ? 1.0
        : morph_timeline_.progress();
    const bool morph_terminal_visible = morph_interactive();
    const bool draw_full_stage =
        animation::workspace_morph_draw_opaque_stage(
            morph_terminal_visible,
            force_native_capture_
        );
    if (draw_full_stage) {
        gtk_snapshot_append_color(snapshot, &black, &widget_bounds);
    }

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
    if (draw_full_stage) {
        gtk_snapshot_append_color(snapshot, &stage_color, &stage_bounds);
    }

    if (!ensure_assets()) {
        gtk_snapshot_append_color(snapshot, &stage_color, &stage_bounds);
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
    const bool viewport_transition = viewport_animation_active_;
    const double viewport_progress = viewport_transition
        ? viewport_animation_progress_
        : 1.0;
    const double viewport_offset = viewport_visual_offset();
    const auto workspace_morph_layout = morph_layout(scale_x, scale_y);
    const auto workspace_morph_frame =
        animation::sample_workspace_morph_frame(
            workspace_morph_layout,
            morph_progress
        );
    const bool morph_transition = !morph_terminal_visible;
    // The overview intentionally spans the full output so the elemental seed
    // can share coordinates with the taskbar runes. The Aether Spine is
    // translucent, though, so normal realm content must stop at the actual
    // 56 px rail edge while the morph is moving. The extra 20 px cap extension
    // is curved artwork, not a permanent opaque column; clipping to it creates
    // a visible dead strip beside the bar.
    const double morph_content_safe_left = std::clamp(
        static_cast<double>(bar::kRailWidth) / std::max(scale_x, 0.000001),
        0.0,
        kReferenceWidth
    );

    gtk_snapshot_save(snapshot);
    const graphene_point_t viewport_translation = GRAPHENE_POINT_INIT(
        0.0F,
        static_cast<float>(viewport_offset)
    );
    gtk_snapshot_translate(snapshot, &viewport_translation);
    if (viewport_transition) {
        gtk_snapshot_push_opacity(
            snapshot,
            static_cast<float>(lerp(0.42, 1.0, viewport_progress))
        );
    }

    for (std::size_t index = 0; index < workspace_state_.size(); ++index) {
        const std::size_t style_index = style_index_for_realm(index);
        const auto& style = kRealms[style_index];
        const auto& style_assets = assets_[style_index];
        const double realm_height = heights[index];
        const double activity = realm_activity(realm_height);
        const auto& morph_band = workspace_morph_frame.bands[index];
        const Rect texture_bounds{
            0.0,
            tops[index],
            kReferenceWidth,
            realm_height,
        };
        // During the morph the elemental seed is allowed to exist at the
        // taskbar rune's real Y coordinate, which can be far outside its final
        // destination band. The frontier/reveal path below owns the actual
        // clipping. Keeping the old destination-only clip is what made the
        // first visible frame suddenly appear at full monitor height.
        const graphene_rect_t realm_clip = GRAPHENE_RECT_INIT(
            0.0F,
            static_cast<float>(morph_transition ? 0.0 : tops[index]),
            static_cast<float>(kReferenceWidth),
            static_cast<float>(morph_transition
                ? kReferenceHeight
                : realm_height)
        );
        gtk_snapshot_push_clip(snapshot, &realm_clip);
        animation::WorkspaceMorphFrontier native_frontier{};
        if (morph_transition) {
            const double native_reveal_width =
                animation::workspace_morph_native_reveal_width(
                    morph_band.reveal_clip.width,
                    morph_renderer_.frame_ready(),
                    workspace_morph_frame.exact_visible
                );
            auto native_reveal = morph_band.reveal_clip;
            native_reveal.width = native_reveal_width;

            // Adjacent morph bands meet on fractional Y coordinates while the
            // overview unfolds. Their antialiased clip edges can otherwise
            // leave a one-pixel pale seam exactly where the settled separator
            // will live. Once unfolding is underway, overlap neighbouring
            // bands by a hair so there is never transparent coverage between
            // them. The overlap is subpixel-small and does not alter the final
            // layout.
            const double seam_overlap = 1.25 * std::clamp(
                (morph_progress - 0.28) / 0.18,
                0.0,
                1.0
            );
            double reveal_top = native_reveal.y;
            double reveal_bottom = native_reveal.y + native_reveal.height;
            if (index > 0U) {
                reveal_top = std::max(0.0, reveal_top - seam_overlap);
            }
            if (index + 1U < workspace_state_.size()) {
                reveal_bottom = std::min(
                    kReferenceHeight,
                    reveal_bottom + seam_overlap
                );
            }
            native_reveal.y = reveal_top;
            native_reveal.height = std::max(0.0, reveal_bottom - reveal_top);

            native_frontier = animation::build_workspace_morph_frontier(
                style_index,
                native_reveal,
                morph_band.rune.x + morph_band.rune.width,
                kReferenceWidth,
                morph_progress
            );
            GskPath* reveal_path = create_workspace_frontier_reveal_path(
                native_frontier
            );
            if (reveal_path != nullptr) {
                // Do not blur the complete reveal polygon. Its top/bottom
                // edges are horizontal by definition, so blurring that fill
                // manufactures pale separator-like bars while the seed unfolds.
                // The dedicated native frontier below already supplies the
                // soft elemental birth/glow without lighting those edges.
                gtk_snapshot_push_fill(
                    snapshot,
                    reveal_path,
                    GSK_FILL_RULE_WINDING
                );
                gsk_path_unref(reveal_path);
            } else {
                const graphene_rect_t morph_clip = GRAPHENE_RECT_INIT(
                    static_cast<float>(native_reveal.x),
                    static_cast<float>(native_reveal.y),
                    static_cast<float>(native_reveal.width),
                    static_cast<float>(native_reveal.height)
                );
                gtk_snapshot_push_clip(snapshot, &morph_clip);
            }
            gtk_snapshot_push_opacity(
                snapshot,
                static_cast<float>(morph_band.realm_opacity)
            );
            const graphene_rect_t morph_content_clip = GRAPHENE_RECT_INIT(
                static_cast<float>(morph_content_safe_left),
                0.0F,
                static_cast<float>(
                    std::max(0.0, kReferenceWidth - morph_content_safe_left)
                ),
                static_cast<float>(kReferenceHeight)
            );
            gtk_snapshot_push_clip(snapshot, &morph_content_clip);
            const graphene_rect_t revealed_stage = GRAPHENE_RECT_INIT(
                0.0F,
                static_cast<float>(tops[index]),
                static_cast<float>(kReferenceWidth),
                static_cast<float>(realm_height)
            );
            gtk_snapshot_append_color(
                snapshot,
                &stage_color,
                &revealed_stage
            );
        }
        append_texture_with_opacity(
            snapshot,
            style_assets.background,
            background_bounds(
                style_assets.background,
                tops[index],
                realm_height,
                activity
            ),
            1.0
        );
        if (morph_transition && !force_native_capture_) {
            // The settled atmosphere is deliberately very dark on the left
            // and almost clear on the right. Removing it during Opening and
            // Closing avoids the old horizontal compositing artifact, but it
            // also makes the overview visibly brighten until the terminal
            // frame restores the grading. Preserve that horizontal grading
            // throughout the morph using a full-height node. The existing
            // morph reveal path owns all Y clipping, so this atmosphere has no
            // per-realm top/bottom rectangle edges that can turn into bright
            // or dark separator-like bands when combined with the texture.
            append_realm_horizontal_atmosphere(
                snapshot,
                0.0,
                kReferenceHeight,
                activity
            );
        } else {
            append_realm_atmosphere(
                snapshot,
                tops[index],
                realm_height,
                activity
            );
        }
        append_texture_with_opacity(
            snapshot,
            style_assets.character,
            character_bounds(
                style_assets.character,
                style,
                tops[index],
                realm_height,
                activity
            ),
            lerp(0.92, 1.0, activity) *
                morph_band.character_opacity
        );
        append_texture_with_opacity(
            snapshot,
            style_assets.compact_overlay,
            texture_bounds,
            (1.0 - activity) * morph_band.card_opacity
        );
        append_texture_with_opacity(
            snapshot,
            style_assets.expanded_overlay,
            texture_bounds,
            activity * morph_band.card_opacity
        );

        if (drag_card_.active &&
            drag_target_index_ == static_cast<int>(index)) {
            const GdkRGBA target_tint{
                static_cast<float>(style.accent.red),
                static_cast<float>(style.accent.green),
                static_cast<float>(style.accent.blue),
                0.105F,
            };
            gtk_snapshot_append_color(snapshot, &target_tint, &realm_clip);

            const GdkRGBA target_edge{
                static_cast<float>(style.accent_soft.red),
                static_cast<float>(style.accent_soft.green),
                static_cast<float>(style.accent_soft.blue),
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
        const bool overflow_mode = overflow_expanded_for(index);
        if (overflow_mode) {
            const auto& realm = workspace_state_[index];
            const double overflow_progress = std::clamp(
                overflow_animation_progress_,
                0.0,
                1.0
            );
            const auto spread_count = std::min(
                realm.windows.size(),
                style_assets.spread_cards.size()
            );
            for (std::size_t slot = 0; slot < spread_count; ++slot) {
                const Rect current = overflow_card_current_rect(
                    slot,
                    realm.windows.size(),
                    tops[index],
                    realm_height,
                    activity,
                    overflow_progress
                );
                double opacity = 1.0;
                if (slot >= 2U) {
                    opacity = transition_ease(std::clamp(
                        (overflow_progress - 0.06) / 0.54,
                        0.0,
                        1.0
                    ));
                }
                const bool dragged_source = drag_card_.active &&
                    drag_card_.source_workspace_id == realm.workspace_id &&
                    drag_card_.address == realm.windows[slot].address;
                append_texture_with_opacity(
                    snapshot,
                    style_assets.spread_cards[slot],
                    current,
                    (dragged_source ? opacity * 0.14 : opacity) *
                        morph_band.card_opacity
                );
            }

            // The +N summary becomes the visual source/sink for hidden cards.
            // Keep it underneath the fan-out for the first half of the motion
            // instead of popping it away on the click frame.
            if (realm.card_count >= kWorkspaceOverviewCardLimit &&
                realm.cards[2].summary && style_assets.cards[2].expanded != nullptr) {
                const double summary_opacity = 1.0 - transition_ease(
                    std::clamp(overflow_progress / 0.62, 0.0, 1.0)
                );
                if (summary_opacity > 0.001) {
                    const auto preview = window_card_visuals(
                        tops[index],
                        realm_height,
                        activity
                    );
                    append_texture_with_opacity(
                        snapshot,
                        style_assets.cards[2].expanded,
                        preview[2].rect,
                        summary_opacity * morph_band.card_opacity
                    );
                }
            }

            const double control_opacity = transition_ease(std::clamp(
                (overflow_progress - 0.48) / 0.34,
                0.0,
                1.0
            ));
            append_overflow_collapse_control(
                widget,
                snapshot,
                overflow_control_rect(tops[index]),
                style,
                control_opacity * morph_band.card_opacity
            );
        } else {
            const auto card_count = std::min(
                workspace_state_[index].card_count,
                style_assets.cards.size()
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
                    drag_card_.source_workspace_id ==
                        workspace_state_[index].workspace_id &&
                    drag_card_.address ==
                        workspace_state_[index].cards[slot].address;
                const double card_opacity = entering ? card_progress : 1.0;
                const Rect compact_current = lerp_rect(
                    compact_start,
                    compact_target,
                    card_progress
                );
                const Rect expanded_current = lerp_rect(
                    expanded_start,
                    expanded_target,
                    card_progress
                );
                append_card_texture_pair(
                    snapshot,
                    style_assets.cards[slot].compact,
                    style_assets.cards[slot].expanded,
                    compact_current,
                    expanded_current,
                    activity,
                    (dragged_source ? card_opacity * 0.14 : card_opacity) *
                        morph_band.card_opacity
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
                        (1.0 - card_progress) * morph_band.card_opacity
                    );
                }
            }
        }

        const Color identity_shadow{
            style.accent.red,
            style.accent.green,
            style.accent.blue,
            0.22,
        };
        int roman_width = 0;
        pango_layout_get_pixel_size(
            style_assets.roman_layout,
            &roman_width,
            nullptr
        );
        constexpr double kRomanRightEdge = 194.0;
        constexpr double kIdentityTextX = 218.0;
        constexpr double kIdentityGap = 24.0;
        const double roman_x = std::max(
            32.0,
            kRomanRightEdge - static_cast<double>(roman_width)
        );
        const double identity_text_x = std::max(
            kIdentityTextX,
            roman_x + static_cast<double>(roman_width) + kIdentityGap
        );
        if (morph_transition) {
            gtk_snapshot_push_opacity(
                snapshot,
                static_cast<float>(morph_band.identity_opacity)
            );
        }
        append_identity_layout(
            snapshot,
            style_assets.roman_layout,
            roman_x,
            tops[index] + realm_height * 0.5,
            style.accent_soft,
            &identity_shadow,
            0.70
        );
        append_identity_layout(
            snapshot,
            style_assets.element_layout,
            identity_text_x,
            tops[index] + realm_height * 0.5 - 14.0,
            {1.0, 1.0, 1.0, 0.90},
            &identity_shadow,
            0.52
        );
        append_identity_layout(
            snapshot,
            style_assets.place_layout,
            identity_text_x,
            tops[index] + realm_height * 0.5 + 17.0,
            {1.0, 1.0, 1.0, 0.52},
            &identity_shadow,
            0.36
        );
        if (morph_transition) gtk_snapshot_pop(snapshot);
        if (morph_transition) {
            gtk_snapshot_pop(snapshot); // morph-content safe clip
            gtk_snapshot_pop(snapshot); // realm opacity
            gtk_snapshot_pop(snapshot); // frontier reveal path/clip
            if (!morph_renderer_.frame_ready()) {
                append_workspace_native_frontier(
                    snapshot,
                    native_frontier,
                    style,
                    style_index
                );
            }
        }
        gtk_snapshot_pop(snapshot);
    }

    const auto selection_target = selected_card_outline_bounds();
    std::optional<graphene_rect_t> selection_current;
    double selection_opacity = selection_target.has_value() ? 1.0 : 0.0;
    if (selection_animation_active_) {
        if (selection_outline_has_start_bounds_) {
            selection_current = selection_target.has_value()
                ? lerp_graphene_rect(
                    selection_outline_start_bounds_,
                    *selection_target,
                    selection_animation_progress_
                )
                : selection_outline_start_bounds_;
        } else if (selection_target.has_value()) {
            selection_current = *selection_target;
        }
        selection_opacity = lerp(
            selection_outline_start_opacity_,
            selection_target.has_value() ? 1.0 : 0.0,
            selection_animation_progress_
        );
    } else if (selection_target.has_value()) {
        selection_current = *selection_target;
    }

    bool selected_card_is_dragged = false;
    if (drag_card_.active && active_index_ >= 0 &&
        active_index_ < static_cast<int>(workspace_state_.size()) &&
        selected_card_index_ >= 0) {
        const auto& selected_realm = workspace_state_[
            static_cast<std::size_t>(active_index_)
        ];
        const auto selected_slot = static_cast<std::size_t>(
            selected_card_index_
        );
        if (overflow_expanded_for(static_cast<std::size_t>(active_index_))) {
            selected_card_is_dragged = selected_slot < selected_realm.windows.size() &&
                selected_realm.workspace_id == drag_card_.source_workspace_id &&
                selected_realm.windows[selected_slot].address == drag_card_.address;
        } else {
            selected_card_is_dragged = selected_slot < selected_realm.card_count &&
                selected_realm.workspace_id == drag_card_.source_workspace_id &&
                selected_realm.cards[selected_slot].address == drag_card_.address;
        }
    }
    if (morph_terminal_visible && selection_current.has_value() &&
        !selected_card_is_dragged &&
        active_index_ >= 0 &&
        active_index_ < static_cast<int>(displayed_heights_.size())) {
        const double selection_activity = realm_activity(
            displayed_heights_[static_cast<std::size_t>(active_index_)]
        );
        append_card_selection_outline(
            snapshot,
            rect_from_graphene(*selection_current),
            realm_style_for_workspace(
                workspace_state_[static_cast<std::size_t>(active_index_)]
                    .workspace_id
            ).accent_soft,
            selection_opacity,
            lerp(kCompactCardRadius, kExpandedCardRadius, selection_activity)
        );
    }

    // Separators and the subtle global vignette are part of the late morph,
    // not a terminal-frame switch. Clip them to the already-revealed width so
    // the wavy lines grow behind the elemental frontier instead of appearing
    // full-width over unrevealed desktop. The same opacity curve runs in
    // reverse on close.
    const double decor_opacity = std::clamp(
        workspace_morph_frame.separator_opacity,
        0.0,
        1.0
    );
    if (decor_opacity > 0.001) {
        gtk_snapshot_push_opacity(
            snapshot,
            static_cast<float>(decor_opacity)
        );
        bool decor_clipped = false;
        if (!workspace_morph_frame.exact_visible) {
            const double decor_right = std::clamp(
                workspace_morph_frame.reveal_right,
                morph_content_safe_left,
                kReferenceWidth
            );
            const graphene_rect_t decor_clip = GRAPHENE_RECT_INIT(
                static_cast<float>(morph_content_safe_left),
                0.0F,
                static_cast<float>(
                    std::max(0.0, decor_right - morph_content_safe_left)
                ),
                static_cast<float>(kReferenceHeight)
            );
            gtk_snapshot_push_clip(snapshot, &decor_clip);
            decor_clipped = true;
        }

        append_ripple_separator(
            snapshot,
            separator_nodes_[0][style_index_for_realm(0)],
            tops[1]
        );
        append_ripple_separator(
            snapshot,
            separator_nodes_[1][style_index_for_realm(1)],
            tops[2]
        );
        append_ripple_separator(
            snapshot,
            separator_nodes_[2][style_index_for_realm(2)],
            tops[3]
        );
        append_global_vignette(snapshot);

        if (decor_clipped) gtk_snapshot_pop(snapshot);
        gtk_snapshot_pop(snapshot);
    }

    // The real taskbar rune artwork is faded by the Aether Spine itself.
    // Drawing another proxy here would sit below the bar and create a ghosted
    // duplicate through its translucent backdrop.
    if (viewport_transition) gtk_snapshot_pop(snapshot);
    gtk_snapshot_restore(snapshot);

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
    if (!morph_interactive() || x < static_cast<double>(bar::kVisualWidth)) return;
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

    if (card_animation_active_ || overflow_animation_active_) return;
    const auto hit = hit_realm_card(
        drag_card_.start_x,
        drag_card_.start_y - viewport_visual_offset(),
        displayed_heights_,
        workspace_state_,
        overflow_workspace_id_,
        overflow_animation_progress_
    );
    if (!hit.has_value()) return;

    const auto& realm = workspace_state_[hit->realm_index];
    const WorkspaceOverviewCard* card = nullptr;
    GdkTexture* spread_texture = nullptr;
    Rect compact{};
    Rect expanded{};
    double drag_activity = 1.0;
    const auto tops = realm_tops(displayed_heights_);
    const double realm_height = displayed_heights_[hit->realm_index];

    if (hit->overflow) {
        if (hit->card_index >= realm.windows.size()) return;
        const auto& spread_assets = assets_[
            style_index_for_realm(hit->realm_index)
        ].spread_cards;
        if (hit->card_index >= spread_assets.size() ||
            spread_assets[hit->card_index] == nullptr) {
            return;
        }
        card = &realm.windows[hit->card_index];
        spread_texture = spread_assets[hit->card_index];
        compact = overflow_card_current_rect(
            hit->card_index,
            realm.windows.size(),
            tops[hit->realm_index],
            realm_height,
            realm_activity(realm_height),
            overflow_animation_progress_
        );
        expanded = compact;
    } else {
        if (hit->card_index >= realm.card_count) return;
        card = &realm.cards[hit->card_index];
        if (card->summary) return;
        compact = scaled_card_rect(
            hit->card_index,
            false,
            tops[hit->realm_index],
            realm_height
        );
        expanded = scaled_card_rect(
            hit->card_index,
            true,
            tops[hit->realm_index],
            realm_height
        );
        drag_activity = realm_activity(realm_height);
    }
    if (card == nullptr || card->address.empty()) return;

    const double viewport_offset = viewport_visual_offset();
    compact.y += viewport_offset;
    expanded.y += viewport_offset;
    if (hit->overflow) {
        drag_card_.assets.expanded = GDK_TEXTURE(g_object_ref(spread_texture));
    } else {
        const auto& source_assets = assets_[
            style_index_for_realm(hit->realm_index)
        ].cards[hit->card_index];
        if (source_assets.compact == nullptr &&
            source_assets.expanded == nullptr) {
            return;
        }
        if (source_assets.compact != nullptr) {
            drag_card_.assets.compact = GDK_TEXTURE(
                g_object_ref(source_assets.compact)
            );
        }
        if (source_assets.expanded != nullptr) {
            drag_card_.assets.expanded = GDK_TEXTURE(
                g_object_ref(source_assets.expanded)
            );
        }
    }
    drag_card_.compact_bounds = {
        compact.x, compact.y, compact.width, compact.height,
    };
    drag_card_.expanded_bounds = {
        expanded.x, expanded.y, expanded.width, expanded.height,
    };
    drag_card_.address = card->address;
    drag_card_.source_workspace_id = realm.workspace_id;
    drag_card_.card_index = hit->card_index;
    drag_card_.activity = drag_activity;
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

    const bool over_taskbar =
        drag_card_.current_x * scale_x < static_cast<double>(bar::kVisualWidth);
    const auto target = over_taskbar
        ? std::optional<int>{}
        : realm_index_at_point(
            drag_card_.current_x,
            drag_card_.current_y - viewport_visual_offset(),
            displayed_heights_
        );
    if (target.has_value()) {
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
        const int source_workspace = drag_card_.source_workspace_id;
        const std::string address = drag_card_.address;
        const int destination_workspace = target_index >= 0 &&
            target_index < static_cast<int>(workspace_state_.size())
            ? workspace_state_[static_cast<std::size_t>(target_index)]
                .workspace_id
            : 0;
        reset_drag();
        gtk_widget_queue_draw(canvas_);

        if (target_index >= 0 &&
            destination_workspace != source_workspace &&
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
    if (canvas_ == nullptr || !morph_interactive() ||
        x < static_cast<double>(bar::kVisualWidth)) return;
    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    if (width <= 0 || height <= 0) return;

    const double scale_x = static_cast<double>(width) / kReferenceWidth;
    const double scale_y = static_cast<double>(height) / kReferenceHeight;
    const double reference_x = x / scale_x;
    const double reference_y = y / scale_y - viewport_visual_offset();
    const auto tops = realm_tops(displayed_heights_);

    if (overflow_workspace_id_ != 0 && overflow_animation_progress_ > 0.55) {
        for (std::size_t index = 0; index < workspace_state_.size(); ++index) {
            if (!overflow_expanded_for(index)) continue;
            if (overflow_control_rect(tops[index]).contains(
                    reference_x, reference_y)) {
                collapse_overflow(false);
                return;
            }
        }
    }

    const auto card_realm = hit_realm_card(
        reference_x,
        reference_y,
        displayed_heights_,
        workspace_state_,
        overflow_workspace_id_,
        overflow_animation_progress_
    );
    if (card_realm.has_value()) {
        const auto& realm = workspace_state_[card_realm->realm_index];
        if (card_realm->overflow) {
            if (card_realm->card_index < realm.windows.size()) {
                const auto& card = realm.windows[card_realm->card_index];
                if (!card.address.empty() && activate_window_) {
                    activate_window_(realm.workspace_id, card.address);
                    hide();
                }
            }
            return;
        }

        const auto& card = realm.cards[card_realm->card_index];
        if (card.summary) {
            expand_overflow(card_realm->realm_index);
            return;
        }
        if (!card.address.empty() && activate_window_) {
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

    if (overflow_workspace_id_ != 0) {
        const auto realm = realm_index_at_point(
            reference_x,
            reference_y,
            displayed_heights_
        );
        if (realm.has_value()) {
            if (*realm != active_index_) select_realm(*realm);
            return;
        }
    }
    hide();
}

void WorkspaceOverviewOverlay::handle_hover(double x, double y) {
    // The overview deliberately spans the full output so its morph geometry can
    // share coordinates with the Aether Spine. The visible bar still owns its
    // entire 76 px strip; never let the surface underneath interpret pointer
    // motion there as a realm hover.
    if (canvas_ == nullptr || drag_card_.active || !morph_interactive() ||
        x < static_cast<double>(bar::kVisualWidth)) return;
    const int width = gtk_widget_get_width(canvas_);
    const int height = gtk_widget_get_height(canvas_);
    if (width <= 0 || height <= 0) return;

    const double scale_x = static_cast<double>(width) / kReferenceWidth;
    const double scale_y = static_cast<double>(height) / kReferenceHeight;
    const double reference_x = x / scale_x;
    const double reference_y = y / scale_y - viewport_visual_offset();
    const auto card = hit_realm_card(
        reference_x,
        reference_y,
        displayed_heights_,
        workspace_state_,
        overflow_workspace_id_,
        overflow_animation_progress_
    );
    if (card.has_value()) {
        select_realm(
            static_cast<int>(card->realm_index),
            static_cast<int>(card->card_index)
        );
        return;
    }

    const auto realm = realm_index_at_point(
        reference_x,
        reference_y,
        displayed_heights_
    );
    if (realm.has_value()) select_realm(*realm);
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
            int roman_workspace_id = static_cast<int>(index) + 1;
            for (const auto& realm : workspace_state_) {
                if (style_index_for_workspace_id(realm.workspace_id) == index) {
                    roman_workspace_id = realm.workspace_id;
                    break;
                }
            }
            const std::string roman = workspace_roman_numeral(
                roman_workspace_id
            );
            assets_[index].roman_layout = create_identity_layout(
                canvas_,
                roman,
                60.0,
                "Georgia",
                0,
                asset_error_
            );
            assets_[index].roman_workspace_id =
                assets_[index].roman_layout != nullptr
                ? roman_workspace_id
                : 0;
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
        realm.roman_workspace_id = 0;
        g_clear_object(&realm.element_layout);
        g_clear_object(&realm.place_layout);
        g_clear_object(&realm.compact_overlay);
        g_clear_object(&realm.expanded_overlay);
        for (auto& card : realm.cards) {
            g_clear_object(&card.compact);
            g_clear_object(&card.expanded);
        }
        for (auto*& card : realm.spread_cards) g_clear_object(&card);
        realm.spread_cards.clear();
    }
}

} // namespace realmheart::ui::workspace
