#include "ui/MonitorResolver.hpp"

#include "core/Command.hpp"
#include "core/TaskExecutor.hpp"
#include "nlohmann_json/json.hpp"
#include "ui/LayerSurface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

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

struct MonitorCache {
    std::mutex mutex;
    std::unordered_map<std::string, double> scales;
    std::string focused_connector;
    std::string signature;
    bool refresh_in_flight = false;
};

MonitorCache& monitor_cache() {
    static MonitorCache cache;
    return cache;
}

std::string hyprland_signature() {
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    return signature != nullptr ? signature : "";
}

void invalidate_cache_for_instance_change(MonitorCache& cache) {
    const std::string signature = hyprland_signature();
    if (cache.signature == signature) return;
    cache.signature = signature;
    cache.scales.clear();
    cache.focused_connector.clear();
}

void refresh_monitor_cache() {
    core::CommandOptions options;
    options.deadline = std::chrono::milliseconds(300);
    options.max_output_bytes = 128 * 1024;
    const auto result = core::run_capture({"hyprctl", "monitors", "-j"}, options);

    std::unordered_map<std::string, double> scales;
    std::string focused_connector;
    bool valid = result.succeeded() && !result.output.empty() && !result.truncated;
    if (valid) {
        try {
            const auto monitors = nlohmann::json::parse(result.output);
            valid = monitors.is_array();
            if (valid) {
                for (const auto& monitor : monitors) {
                    if (!monitor.is_object()) continue;
                    const std::string name = monitor.value("name", std::string{});
                    const double scale = monitor.value("scale", 0.0);
                    if (!name.empty() && std::isfinite(scale) && scale > 0.0) {
                        scales[name] = scale;
                    }
                    if (focused_connector.empty() &&
                        monitor.value("focused", false) && !name.empty()) {
                        focused_connector = name;
                    }
                }
            }
        } catch (const nlohmann::json::exception&) {
            valid = false;
        }
    }

    auto& cache = monitor_cache();
    std::lock_guard lock(cache.mutex);
    invalidate_cache_for_instance_change(cache);
    if (valid) {
        cache.scales = std::move(scales);
        cache.focused_connector = std::move(focused_connector);
    }
    cache.refresh_in_flight = false;
}

void request_monitor_cache_refresh() {
    auto& cache = monitor_cache();
    {
        std::lock_guard lock(cache.mutex);
        invalidate_cache_for_instance_change(cache);
        if (cache.refresh_in_flight) return;
        cache.refresh_in_flight = true;
    }
    if (!core::shared_task_executor().post(
            refresh_monitor_cache,
            "hyprland-monitor-state"
        )) {
        std::lock_guard lock(cache.mutex);
        cache.refresh_in_flight = false;
    }
}

double hyprland_scale_for_connector(std::string_view connector) {
    request_monitor_cache_refresh();
    auto& cache = monitor_cache();
    std::lock_guard lock(cache.mutex);
    const auto found = cache.scales.find(std::string(connector));
    return found == cache.scales.end() ? 0.0 : found->second;
}

std::string focused_connector_from_hyprland() {
    request_monitor_cache_refresh();
    auto& cache = monitor_cache();
    std::lock_guard lock(cache.mutex);
    return cache.focused_connector;
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
