#include "screenshot/MonitorResolver.hpp"

#include "core/Command.hpp"
#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <future>
#include <utility>

namespace realmheart::screenshot {
namespace {

using json = nlohmann::json;

MonitorResolveResult failure(std::string message) {
    MonitorResolveResult result;
    result.error = std::move(message);
    return result;
}

bool transformed_dimensions_swap(int transform) {
    // Hyprland/wl_output transforms 1, 3, 5 and 7 are quarter-turn variants.
    return transform == 1 || transform == 3 || transform == 5 || transform == 7;
}

std::optional<MonitorTarget> parse_monitor_target(const json& monitor) {
    if (!monitor.is_object()) return std::nullopt;

    const std::string connector = monitor.value("name", std::string{});
    const int width = monitor.value("width", 0);
    const int height = monitor.value("height", 0);
    double scale = monitor.value("scale", 1.0);
    const int transform = monitor.value("transform", 0);

    if (connector.empty() || width <= 0 || height <= 0) return std::nullopt;
    if (!std::isfinite(scale) || scale <= std::numeric_limits<double>::epsilon()) {
        scale = 1.0;
    }

    double logical_width = static_cast<double>(width);
    double logical_height = static_cast<double>(height);
    if (transformed_dimensions_swap(transform)) {
        std::swap(logical_width, logical_height);
    }
    logical_width /= scale;
    logical_height /= scale;

    MonitorTarget target;
    target.connector = connector;
    target.id = monitor.value("id", -1);
    target.physical_width = width;
    target.physical_height = height;
    target.scale = scale;
    target.transform = transform;
    target.layout_x = monitor.value("x", 0.0);
    target.layout_y = monitor.value("y", 0.0);
    target.logical_width = logical_width;
    target.logical_height = logical_height;

    if (const auto workspace = monitor.find("activeWorkspace");
        workspace != monitor.end() && workspace->is_object() &&
        workspace->contains("id") && (*workspace)["id"].is_number_integer()) {
        target.active_workspace_id = (*workspace)["id"].get<int>();
    }
    if (const auto workspace = monitor.find("specialWorkspace");
        workspace != monitor.end() && workspace->is_object() &&
        workspace->contains("id") && (*workspace)["id"].is_number_integer()) {
        target.special_workspace_id = (*workspace)["id"].get<int>();
    }
    return target;
}

bool cursor_inside_monitor(const json& monitor, double cursor_x, double cursor_y) {
    if (!monitor.is_object()) return false;

    const double origin_x = monitor.value("x", 0.0);
    const double origin_y = monitor.value("y", 0.0);
    double width = monitor.value("width", 0.0);
    double height = monitor.value("height", 0.0);
    double scale = monitor.value("scale", 1.0);
    const int transform = monitor.value("transform", 0);

    if (!std::isfinite(scale) || scale <= std::numeric_limits<double>::epsilon()) {
        scale = 1.0;
    }
    if (transformed_dimensions_swap(transform)) std::swap(width, height);

    // Hyprland's monitor x/y are layout-space coordinates while width/height are
    // physical output dimensions. Convert the extents into layout-space before
    // hit-testing, which keeps monitor-under-cursor correct under fractional scale.
    width /= scale;
    height /= scale;

    return width > 0.0 && height > 0.0 &&
        cursor_x >= origin_x && cursor_x < origin_x + width &&
        cursor_y >= origin_y && cursor_y < origin_y + height;
}

} // namespace

MonitorResolveResult MonitorResolver::from_hyprland_json(
    std::string_view cursor_json,
    std::string_view monitors_json
) {
    try {
        const auto cursor = json::parse(cursor_json);
        const auto monitors = json::parse(monitors_json);
        if (!cursor.is_object() || !monitors.is_array()) {
            return failure("Hyprland returned malformed cursor/monitor JSON");
        }

        const double cursor_x = cursor.value("x", std::numeric_limits<double>::quiet_NaN());
        const double cursor_y = cursor.value("y", std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(cursor_x) || !std::isfinite(cursor_y)) {
            return failure("Hyprland cursor position is unavailable");
        }

        for (const auto& monitor : monitors) {
            if (!cursor_inside_monitor(monitor, cursor_x, cursor_y)) continue;
            if (auto target = parse_monitor_target(monitor)) {
                MonitorResolveResult result;
                result.monitor = std::move(target);
                return result;
            }
        }

        // The cursor can briefly land outside every reported layout rectangle while
        // an output is being reconfigured. Falling back to Hyprland's focused output
        // is preferable to capturing an arbitrary monitor in that transient state.
        for (const auto& monitor : monitors) {
            if (!monitor.is_object() || !monitor.value("focused", false)) continue;
            if (auto target = parse_monitor_target(monitor)) {
                MonitorResolveResult result;
                result.monitor = std::move(target);
                return result;
            }
        }

        return failure("no active Hyprland monitor contains the cursor");
    } catch (const json::exception&) {
        return failure("unable to parse Hyprland cursor/monitor JSON");
    }
}

MonitorResolveResult MonitorResolver::detect_under_cursor() {
    if (!realmheart::core::command_exists("hyprctl")) {
        return failure("hyprctl not found; Realmheart screenshot requires Hyprland's hyprctl");
    }

    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::seconds(2);
    options.max_output_bytes = 512 * 1024;

    auto cursor_future = std::async(std::launch::async, [options]() {
        return realmheart::core::run_capture(
            {"hyprctl", "-j", "cursorpos"},
            options
        );
    });
    auto monitors_future = std::async(std::launch::async, [options]() {
        return realmheart::core::run_capture(
            {"hyprctl", "-j", "monitors"},
            options
        );
    });

    const auto cursor = cursor_future.get();
    const auto monitors = monitors_future.get();

    if (!cursor.succeeded() || cursor.output.empty() || cursor.truncated) {
        return failure(realmheart::core::command_failure_detail(
            cursor,
            "hyprctl cursorpos failed"
        ));
    }
    if (!monitors.succeeded() || monitors.output.empty() || monitors.truncated) {
        return failure(realmheart::core::command_failure_detail(
            monitors,
            "hyprctl monitors failed"
        ));
    }

    return from_hyprland_json(cursor.output, monitors.output);
}

} // namespace realmheart::screenshot
