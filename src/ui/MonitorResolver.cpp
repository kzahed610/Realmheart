#include "ui/MonitorResolver.hpp"

#include "ui/LayerSurface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace realmheart::ui {
namespace {

int configured_monitor_index() noexcept {
    const char* configured = g_getenv("REALMHEART_MONITOR_INDEX");
    if (configured == nullptr || *configured == '\0') return 0;

    char* end = nullptr;
    const gint64 parsed = g_ascii_strtoll(configured, &end, 10);
    if (end == configured || end == nullptr || *end != '\0' ||
        parsed < 0 || parsed > std::numeric_limits<int>::max()) {
        return 0;
    }
    return static_cast<int>(parsed);
}

double hyprland_scale_for_connector(std::string_view connector) {
    if (connector.empty()) return 0.0;

    GError* error = nullptr;
    gchar* stdout_buf = nullptr;
    gchar* stderr_buf = nullptr;
    gint exit_status = 0;
    const gboolean spawned = g_spawn_command_line_sync(
        "hyprctl monitors -j",
        &stdout_buf,
        &stderr_buf,
        &exit_status,
        &error
    );

    double result = 0.0;
    if (spawned && error == nullptr && exit_status == 0 && stdout_buf != nullptr) {
        const std::string json(stdout_buf);
        std::size_t cursor = 0;
        while (cursor < json.size()) {
            const auto object_start = json.find('{', cursor);
            if (object_start == std::string::npos) break;

            int depth = 0;
            std::size_t object_end = object_start;
            for (; object_end < json.size(); ++object_end) {
                if (json[object_end] == '{') ++depth;
                else if (json[object_end] == '}') {
                    --depth;
                    if (depth == 0) break;
                }
            }
            if (object_end >= json.size()) break;

            const std::string_view object(
                json.data() + object_start,
                object_end - object_start + 1
            );
            const auto name_key = object.find("\"name\"");
            const auto colon = name_key == std::string_view::npos
                ? std::string_view::npos
                : object.find(':', name_key);
            const auto quote = colon == std::string_view::npos
                ? std::string_view::npos
                : object.find('"', colon + 1);
            const auto end_quote = quote == std::string_view::npos
                ? std::string_view::npos
                : object.find('"', quote + 1);
            if (quote != std::string_view::npos &&
                end_quote != std::string_view::npos &&
                object.substr(quote + 1, end_quote - quote - 1) == connector) {
                const auto scale_key = object.find("\"scale\"");
                const auto scale_colon = scale_key == std::string_view::npos
                    ? std::string_view::npos
                    : object.find(':', scale_key);
                if (scale_colon != std::string_view::npos) {
                    const char* start = object.data() + scale_colon + 1;
                    char* end = nullptr;
                    const double parsed = g_ascii_strtod(start, &end);
                    if (end != start && std::isfinite(parsed) && parsed > 0.0) {
                        result = parsed;
                    }
                }
                break;
            }
            cursor = object_end + 1;
        }
    }

    if (error != nullptr) g_error_free(error);
    if (stdout_buf != nullptr) g_free(stdout_buf);
    if (stderr_buf != nullptr) g_free(stderr_buf);
    return result;
}

std::string focused_connector_from_hyprland() {
    GError* error = nullptr;
    gchar* stdout_buf = nullptr;
    gchar* stderr_buf = nullptr;
    gint exit_status = 0;

    const gboolean spawned = g_spawn_command_line_sync(
        "hyprctl monitors -j",
        &stdout_buf,
        &stderr_buf,
        &exit_status,
        &error
    );
    std::string connector;
    if (spawned && error == nullptr && exit_status == 0 && stdout_buf != nullptr) {
        const std::string json(stdout_buf);
        std::size_t cursor = 0;
        while (cursor < json.size()) {
            const auto object_start = json.find('{', cursor);
            if (object_start == std::string::npos) break;

            int depth = 0;
            std::size_t object_end = object_start;
            for (; object_end < json.size(); ++object_end) {
                if (json[object_end] == '{') ++depth;
                else if (json[object_end] == '}') {
                    --depth;
                    if (depth == 0) break;
                }
            }
            if (object_end >= json.size()) break;

            const std::string_view object(
                json.data() + object_start,
                object_end - object_start + 1
            );
            const bool focused =
                object.find("\"focused\": true") != std::string_view::npos ||
                object.find("\"focused\":true") != std::string_view::npos;
            if (focused) {
                const auto name_key = object.find("\"name\"");
                if (name_key != std::string_view::npos) {
                    const auto colon = object.find(':', name_key);
                    const auto quote = colon == std::string_view::npos
                        ? std::string_view::npos
                        : object.find('"', colon + 1);
                    const auto end_quote = quote == std::string_view::npos
                        ? std::string_view::npos
                        : object.find('"', quote + 1);
                    if (quote != std::string_view::npos &&
                        end_quote != std::string_view::npos) {
                        connector.assign(
                            object.substr(quote + 1, end_quote - quote - 1)
                        );
                    }
                }
                break;
            }
            cursor = object_end + 1;
        }
    }

    if (error != nullptr) g_error_free(error);
    if (stdout_buf != nullptr) g_free(stdout_buf);
    if (stderr_buf != nullptr) g_free(stderr_buf);
    return connector;
}

} // namespace

int monitor_count(GdkDisplay* display) noexcept {
    if (display == nullptr) return 0;
    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors == nullptr) return 0;
    const guint count = g_list_model_get_n_items(monitors);
    return count > static_cast<guint>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(count);
}

std::optional<core::MonitorContext> monitor_context_for_index(
    GdkDisplay* display,
    int monitor_index
) {
    if (display == nullptr) return std::nullopt;
    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors == nullptr) return std::nullopt;

    const guint count = g_list_model_get_n_items(monitors);
    if (count == 0) return std::nullopt;
    const int bounded_index = monitor_index >= 0 &&
        static_cast<guint>(monitor_index) < count
        ? monitor_index
        : 0;

    GdkMonitor* monitor = GDK_MONITOR(
        g_list_model_get_item(monitors, static_cast<guint>(bounded_index))
    );
    if (monitor == nullptr) return std::nullopt;

    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    const char* connector_raw = gdk_monitor_get_connector(monitor);
    const std::string connector = connector_raw != nullptr ? connector_raw : "";
    double scale = static_cast<double>(
        std::max(gdk_monitor_get_scale_factor(monitor), 1)
    );
    const double compositor_scale = hyprland_scale_for_connector(connector);
    if (compositor_scale > 0.0) scale = compositor_scale;
    g_object_unref(monitor);
    if (geometry.width <= 0 || geometry.height <= 0) return std::nullopt;

    return core::monitor_context_for_geometry(
        bounded_index,
        geometry.x,
        geometry.y,
        geometry.width,
        geometry.height,
        scale
    );
}

std::optional<core::MonitorContext> monitor_context_for_widget(
    GtkWidget* widget,
    int monitor_index
) {
    if (widget == nullptr || !gtk_widget_get_realized(widget)) return std::nullopt;
    GdkDisplay* display = gtk_widget_get_display(widget);
    if (display == nullptr) return std::nullopt;

    if (monitor_index >= 0) {
        return monitor_context_for_index(display, monitor_index);
    }

    GdkMonitor* assigned = resolve_layer_surface_monitor(widget, monitor_index);
    if (assigned == nullptr) return std::nullopt;

    GListModel* monitors = gdk_display_get_monitors(display);
    const guint count = monitors != nullptr ? g_list_model_get_n_items(monitors) : 0;
    int assigned_index = 0;
    for (guint index = 0; index < count; ++index) {
        GdkMonitor* candidate = GDK_MONITOR(g_list_model_get_item(monitors, index));
        const bool same = candidate == assigned;
        if (candidate != nullptr) g_object_unref(candidate);
        if (same) {
            assigned_index = static_cast<int>(index);
            break;
        }
    }
    g_object_unref(assigned);
    return monitor_context_for_index(display, assigned_index);
}

std::string monitor_connector_for_index(
    GdkDisplay* display,
    int monitor_index
) {
    if (display == nullptr) return {};
    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors == nullptr) return {};
    const guint count = g_list_model_get_n_items(monitors);
    if (monitor_index < 0 || static_cast<guint>(monitor_index) >= count) return {};

    GdkMonitor* monitor = GDK_MONITOR(
        g_list_model_get_item(monitors, static_cast<guint>(monitor_index))
    );
    if (monitor == nullptr) return {};
    const char* connector = gdk_monitor_get_connector(monitor);
    std::string result = connector != nullptr ? connector : "";
    g_object_unref(monitor);
    return result;
}

int focused_monitor_index(GdkDisplay* display) {
    const int count = monitor_count(display);
    if (count <= 0) return 0;

    const std::string focused_connector = focused_connector_from_hyprland();
    if (!focused_connector.empty()) {
        GListModel* monitors = gdk_display_get_monitors(display);
        for (int index = 0; index < count; ++index) {
            GdkMonitor* monitor = GDK_MONITOR(
                g_list_model_get_item(monitors, static_cast<guint>(index))
            );
            if (monitor == nullptr) continue;
            const char* connector = gdk_monitor_get_connector(monitor);
            const bool match = connector != nullptr && focused_connector == connector;
            g_object_unref(monitor);
            if (match) return index;
        }
    }

    const int configured = configured_monitor_index();
    return configured >= 0 && configured < count ? configured : 0;
}

} // namespace realmheart::ui
