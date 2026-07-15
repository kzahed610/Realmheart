#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include "ui/AssetResolver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::ui::bar::widgets {
namespace {

struct ColorKey {
    std::array<std::uint8_t, 4> primary{};
    std::array<std::uint8_t, 4> accent{};
    std::array<std::uint8_t, 4> on_accent{};

    bool operator==(const ColorKey&) const = default;
};

std::array<std::uint8_t, 4> quantize(const GdkRGBA& color) {
    const auto channel = [](float value) {
        return static_cast<std::uint8_t>(std::clamp(
            std::lround(static_cast<double>(value) * 255.0), 0L, 255L
        ));
    };
    return {channel(color.red), channel(color.green), channel(color.blue), channel(color.alpha)};
}

std::string css_hex(const std::array<std::uint8_t, 4>& color) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(7, '#');
    for (std::size_t index = 0; index < 3; ++index) {
        result[1 + (index * 2)] = digits[color[index] >> 4];
        result[2 + (index * 2)] = digits[color[index] & 0x0f];
    }
    return result;
}

void replace_all(std::string& text, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        text.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

std::string resolve_tokens(std::string source, const ColorKey& colors) {
    const std::string primary = css_hex(colors.primary);
    const std::string accent = css_hex(colors.accent);
    const std::string on_accent = css_hex(colors.on_accent);

    replace_all(source, "var(--rh-icon-primary, currentColor)", primary);
    replace_all(source, "var(--rh-icon-accent, #ffd66b)", accent);
    replace_all(source, "var(--rh-icon-on-accent, #18151f)", on_accent);
    replace_all(source, "var(--rh-icon-primary)", primary);
    replace_all(source, "var(--rh-icon-accent)", accent);
    replace_all(source, "var(--rh-icon-on-accent)", on_accent);

    const std::string injected_style =
        "<style>:root{--rh-icon-primary:" + primary +
        ";--rh-icon-accent:" + accent +
        ";--rh-icon-on-accent:" + on_accent + ";}</style>";
    const std::size_t svg_open = source.find('>');
    if (svg_open != std::string::npos) source.insert(svg_open + 1, injected_style);

    replace_all(source, "currentColor", primary);
    return source;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    );
}

} // namespace

struct ThemedSvgRenderState {
    GtkWidget* drawing_area = nullptr;
    GtkWidget* accent_probe = nullptr;
    GtkWidget* on_accent_probe = nullptr;
    std::string source;
    std::string relative_path;
    std::vector<std::uint32_t> pixels;
    cairo_surface_t* surface = nullptr;
    ColorKey cached_colors{};
    int cached_width = 0;
    int cached_height = 0;
    bool cache_valid = false;

    ~ThemedSvgRenderState() {
        if (surface != nullptr) cairo_surface_destroy(surface);
    }
};

namespace {

void destroy_state(gpointer raw) {
    delete static_cast<ThemedSvgRenderState*>(raw);
}

bool rebuild_surface(
    ThemedSvgRenderState& state,
    int width,
    int height,
    const ColorKey& colors
) {
    if (state.source.empty() || width <= 0 || height <= 0) return false;

    GError* error = nullptr;
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("svg", &error);
    if (loader == nullptr) {
        g_clear_error(&error);
        return false;
    }

    gdk_pixbuf_loader_set_size(loader, width, height);
    const std::string rendered = resolve_tokens(state.source, colors);
    const gboolean wrote = gdk_pixbuf_loader_write(
        loader,
        reinterpret_cast<const guchar*>(rendered.data()),
        rendered.size(),
        &error
    );
    const gboolean closed = wrote ? gdk_pixbuf_loader_close(loader, &error) : FALSE;
    if (!wrote || !closed) {
        g_clear_error(&error);
        g_object_unref(loader);
        return false;
    }

    GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf == nullptr) {
        g_object_unref(loader);
        return false;
    }

    const int image_width = gdk_pixbuf_get_width(pixbuf);
    const int image_height = gdk_pixbuf_get_height(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar* source_pixels = gdk_pixbuf_read_pixels(pixbuf);
    if (image_width <= 0 || image_height <= 0 || source_pixels == nullptr ||
        (channels != 3 && channels != 4)) {
        g_object_unref(loader);
        return false;
    }

    if (state.surface != nullptr) {
        cairo_surface_destroy(state.surface);
        state.surface = nullptr;
    }
    state.pixels.assign(
        static_cast<std::size_t>(image_width) * static_cast<std::size_t>(image_height),
        0U
    );

    for (int y = 0; y < image_height; ++y) {
        const guchar* row = source_pixels + (static_cast<std::size_t>(y) * rowstride);
        for (int x = 0; x < image_width; ++x) {
            const guchar* pixel = row + (static_cast<std::size_t>(x) * channels);
            const std::uint32_t alpha = channels == 4 ? pixel[3] : 255U;
            const std::uint32_t red = (static_cast<std::uint32_t>(pixel[0]) * alpha + 127U) / 255U;
            const std::uint32_t green = (static_cast<std::uint32_t>(pixel[1]) * alpha + 127U) / 255U;
            const std::uint32_t blue = (static_cast<std::uint32_t>(pixel[2]) * alpha + 127U) / 255U;
            state.pixels[
                static_cast<std::size_t>(y) * static_cast<std::size_t>(image_width) +
                static_cast<std::size_t>(x)
            ] = (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
        }
    }

    state.surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(state.pixels.data()),
        CAIRO_FORMAT_ARGB32,
        image_width,
        image_height,
        image_width * static_cast<int>(sizeof(std::uint32_t))
    );
    if (cairo_surface_status(state.surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(state.surface);
        state.surface = nullptr;
        state.pixels.clear();
        g_object_unref(loader);
        return false;
    }

    cairo_surface_mark_dirty(state.surface);
    state.cached_colors = colors;
    state.cached_width = width;
    state.cached_height = height;
    state.cache_valid = true;
    g_object_unref(loader);
    return true;
}

void draw_icon(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    auto& state = *static_cast<ThemedSvgRenderState*>(raw);

    GdkRGBA primary{};
    GdkRGBA accent{};
    GdkRGBA on_accent{};
    gtk_widget_get_color(GTK_WIDGET(area), &primary);
    gtk_widget_get_color(state.accent_probe, &accent);
    gtk_widget_get_color(state.on_accent_probe, &on_accent);
    const ColorKey colors{
        .primary = quantize(primary),
        .accent = quantize(accent),
        .on_accent = quantize(on_accent),
    };

    if (!state.cache_valid || state.cached_width != width || state.cached_height != height ||
        !(state.cached_colors == colors)) {
        if (!rebuild_surface(state, width, height, colors)) return;
    }

    cairo_save(cr);
    cairo_set_source_surface(cr, state.surface, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

} // namespace

ThemedSvgIcon::ThemedSvgIcon(std::string relative_path, int pixels) {
    widget_ = gtk_overlay_new();
    gtk_widget_add_css_class(widget_, "realmheart-themed-svg-icon");
    gtk_widget_set_halign(widget_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(widget_, GTK_ALIGN_CENTER);

    state_ = new ThemedSvgRenderState();
    g_object_set_data_full(
        G_OBJECT(widget_),
        "realmheart-themed-svg-state",
        state_,
        destroy_state
    );

    state_->drawing_area = gtk_drawing_area_new();
    gtk_widget_add_css_class(state_->drawing_area, "realmheart-bar-svg-icon");
    gtk_widget_set_can_target(state_->drawing_area, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(widget_), state_->drawing_area);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(state_->drawing_area),
        draw_icon,
        state_,
        nullptr
    );

    state_->accent_probe = gtk_label_new(nullptr);
    gtk_widget_add_css_class(state_->accent_probe, "realmheart-icon-accent-probe");
    gtk_widget_set_opacity(state_->accent_probe, 0.0);
    gtk_widget_set_can_target(state_->accent_probe, FALSE);
    gtk_widget_set_size_request(state_->accent_probe, 1, 1);
    gtk_overlay_add_overlay(GTK_OVERLAY(widget_), state_->accent_probe);

    state_->on_accent_probe = gtk_label_new(nullptr);
    gtk_widget_add_css_class(state_->on_accent_probe, "realmheart-icon-on-accent-probe");
    gtk_widget_set_opacity(state_->on_accent_probe, 0.0);
    gtk_widget_set_can_target(state_->on_accent_probe, FALSE);
    gtk_widget_set_size_request(state_->on_accent_probe, 1, 1);
    gtk_overlay_add_overlay(GTK_OVERLAY(widget_), state_->on_accent_probe);

    set_size(pixels);
    if (!relative_path.empty()) static_cast<void>(set_icon(std::move(relative_path)));
}

bool ThemedSvgIcon::set_icon(std::string relative_path) {
    state_->relative_path = std::move(relative_path);
    state_->source.clear();
    state_->cache_valid = false;

    const auto resolved = realmheart::ui::resolve_project_asset(state_->relative_path);
    if (resolved) state_->source = read_file(*resolved);
    gtk_widget_queue_draw(state_->drawing_area);
    return !state_->source.empty();
}

void ThemedSvgIcon::set_size(int pixels) {
    const int size = std::clamp(pixels, 12, 64);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(state_->drawing_area), size);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(state_->drawing_area), size);
    gtk_widget_set_size_request(widget_, size, size);
    state_->cache_valid = false;
    gtk_widget_queue_draw(state_->drawing_area);
}

void ThemedSvgIcon::add_css_class(const char* css_class) {
    if (css_class == nullptr || *css_class == '\0') return;
    gtk_widget_add_css_class(widget_, css_class);
}

} // namespace realmheart::ui::bar::widgets
