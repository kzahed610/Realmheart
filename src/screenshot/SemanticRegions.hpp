#pragma once

#include "core/Command.hpp"
#include "nlohmann_json/json.hpp"
#include "screenshot/MonitorResolver.hpp"
#include "screenshot/SelectionGeometry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::screenshot {

enum class SemanticRegionSource {
    Content,
    Layer,
    Window,
};

struct SemanticRegion {
    SelectionRect rect;
    SemanticRegionSource source = SemanticRegionSource::Window;
    std::string label;
    int priority = 100;
    int focus_history_id = 1000000;
};

struct SemanticRegionSnapshot {
    bool available = false;
    double monitor_width = 0.0;
    double monitor_height = 0.0;
    std::vector<SemanticRegion> regions;
    std::string error;
};

class SemanticRegionDetector {
public:
    static SemanticRegionSnapshot read(const MonitorTarget& monitor) {
        if (!realmheart::core::command_exists("hyprctl")) {
            return unavailable("hyprctl not found; window/layer smart targeting is unavailable");
        }

        realmheart::core::CommandOptions options;
        options.deadline = std::chrono::seconds(2);
        options.max_output_bytes = 1024 * 1024;

        auto clients_future = std::async(std::launch::async, [options]() {
            return realmheart::core::run_capture(
                {"hyprctl", "-j", "clients"},
                options
            );
        });
        auto layers_future = std::async(std::launch::async, [options]() {
            return realmheart::core::run_capture(
                {"hyprctl", "-j", "layers"},
                options
            );
        });

        const auto clients = clients_future.get();
        const auto layers = layers_future.get();

        const bool clients_command_ok =
            clients.succeeded() && !clients.output.empty() && !clients.truncated;
        const bool layers_command_ok =
            layers.succeeded() && !layers.output.empty() && !layers.truncated;

        const auto valid_json_shape = [](std::string_view input, bool expect_array) {
            try {
                const auto parsed = json::parse(input);
                return expect_array ? parsed.is_array() : parsed.is_object();
            } catch (const json::exception&) {
                return false;
            }
        };

        const bool clients_ok = clients_command_ok &&
            valid_json_shape(clients.output, true);
        const bool layers_ok = layers_command_ok &&
            valid_json_shape(layers.output, false);

        const auto clients_error = [&]() {
            if (!clients_command_ok) {
                return realmheart::core::command_failure_detail(
                    clients,
                    "hyprctl clients failed"
                );
            }
            return std::string{"Hyprland clients returned malformed JSON"};
        };
        const auto layers_error = [&]() {
            if (!layers_command_ok) {
                return realmheart::core::command_failure_detail(
                    layers,
                    "hyprctl layers failed"
                );
            }
            return std::string{"Hyprland layers returned malformed JSON"};
        };

        if (!clients_ok && !layers_ok) {
            return unavailable(clients_error() + "; " + layers_error());
        }

        const std::string_view clients_json =
            clients_ok ? std::string_view(clients.output) : std::string_view("[]");
        const std::string_view layers_json =
            layers_ok ? std::string_view(layers.output) : std::string_view("{}");

        auto snapshot = from_hyprland_json(monitor, clients_json, layers_json);
        if (!snapshot.error.empty()) return snapshot;

        if (!clients_ok) snapshot.error = clients_error();
        if (!layers_ok) {
            if (!snapshot.error.empty()) snapshot.error += "; ";
            snapshot.error += layers_error();
        }
        return snapshot;
    }

    static SemanticRegionSnapshot from_hyprland_json(
        const MonitorTarget& monitor,
        std::string_view clients_json,
        std::string_view layers_json
    ) {
        using json = nlohmann::json;

        if (monitor.logical_width <= 0.0 || monitor.logical_height <= 0.0) {
            return unavailable("monitor layout geometry is unavailable");
        }

        try {
            const auto clients = json::parse(clients_json);
            const auto layers = json::parse(layers_json);
            if (!clients.is_array()) {
                return unavailable("unable to parse Hyprland clients");
            }

            SemanticRegionSnapshot snapshot;
            snapshot.available = true;
            snapshot.monitor_width = monitor.logical_width;
            snapshot.monitor_height = monitor.logical_height;

            for (const auto& client : clients) {
                if (!client.is_object()) continue;
                if (!json_boolish(client, "mapped", true)) continue;
                if (json_boolish(client, "hidden", false)) continue;

                if (monitor.id >= 0 && client.contains("monitor") &&
                    client["monitor"].is_number_integer() &&
                    client["monitor"].get<int>() != monitor.id) {
                    continue;
                }

                // Newer Hyprland exposes the compositor's actual visibility state.
                // Prefer that over trying to reconstruct occlusion/fullscreen state
                // ourselves. Older versions fall back to workspace membership.
                const auto visible = optional_boolish(client, "visible");
                if (visible.has_value()) {
                    if (!*visible) continue;
                } else {
                    const int workspace_id = workspace_id_of(client);
                    const bool on_regular_workspace =
                        monitor.active_workspace_id > 0 &&
                        workspace_id == monitor.active_workspace_id;
                    const bool on_special_workspace =
                        monitor.special_workspace_id != 0 &&
                        workspace_id == monitor.special_workspace_id;
                    const bool pinned = json_boolish(client, "pinned", false);
                    if (!on_regular_workspace && !on_special_workspace && !pinned) {
                        continue;
                    }
                }

                const auto global_rect = client_rect(client);
                if (!global_rect) continue;
                const auto local_rect = clip_to_monitor(
                    global_rect->x - monitor.layout_x,
                    global_rect->y - monitor.layout_y,
                    global_rect->width,
                    global_rect->height,
                    monitor.logical_width,
                    monitor.logical_height
                );
                if (!local_rect) continue;

                const bool floating = json_boolish(client, "floating", false);
                const bool fullscreen = json_intish(client, "fullscreen", 0) != 0 ||
                    json_intish(client, "fullscreenClient", 0) != 0;
                const bool over_fullscreen = json_boolish(client, "overFullscreen", false);
                const int focus_history = json_intish(
                    client,
                    "focusHistoryID",
                    1000000
                );
                const bool active = focus_history == 0;

                std::string label = client.value("title", std::string{});
                if (label.empty()) label = client.value("class", std::string{});
                if (label.empty()) label = "Window";

                int priority = 30;
                if (floating) priority -= 6;
                if (over_fullscreen) priority -= 4;
                if (active) priority -= 3;
                if (fullscreen) priority -= 2;

                snapshot.regions.push_back(SemanticRegion{
                    .rect = *local_rect,
                    .source = SemanticRegionSource::Window,
                    .label = std::move(label),
                    .priority = priority,
                    .focus_history_id = focus_history,
                });
            }

            append_layer_regions(snapshot, monitor, layers);

            std::stable_sort(
                snapshot.regions.begin(),
                snapshot.regions.end(),
                [](const SemanticRegion& left, const SemanticRegion& right) {
                    if (left.priority != right.priority) {
                        return left.priority < right.priority;
                    }
                    if (left.source == SemanticRegionSource::Window &&
                        right.source == SemanticRegionSource::Window &&
                        left.focus_history_id != right.focus_history_id) {
                        return left.focus_history_id < right.focus_history_id;
                    }
                    const double left_area = left.rect.width * left.rect.height;
                    const double right_area = right.rect.width * right.rect.height;
                    if (std::abs(left_area - right_area) > 0.5) {
                        return left_area < right_area;
                    }
                    return left.label < right.label;
                }
            );

            return snapshot;
        } catch (const json::exception&) {
            return unavailable("unable to parse Hyprland semantic-region JSON");
        }
    }

private:
    using json = nlohmann::json;

    struct RawRect {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    static SemanticRegionSnapshot unavailable(std::string error) {
        SemanticRegionSnapshot snapshot;
        snapshot.error = std::move(error);
        return snapshot;
    }

    static std::optional<bool> optional_boolish(
        const json& object,
        std::string_view key
    ) {
        const auto iterator = object.find(std::string(key));
        if (iterator == object.end()) return std::nullopt;
        if (iterator->is_boolean()) return iterator->get<bool>();
        if (iterator->is_number_integer()) return iterator->get<int>() != 0;
        return std::nullopt;
    }

    static bool json_boolish(
        const json& object,
        std::string_view key,
        bool fallback
    ) {
        const auto value = optional_boolish(object, key);
        return value.value_or(fallback);
    }

    static int json_intish(
        const json& object,
        std::string_view key,
        int fallback
    ) {
        const auto iterator = object.find(std::string(key));
        if (iterator == object.end()) return fallback;
        if (iterator->is_number_integer()) return iterator->get<int>();
        if (iterator->is_boolean()) return iterator->get<bool>() ? 1 : 0;
        return fallback;
    }

    static int workspace_id_of(const json& client) {
        const auto iterator = client.find("workspace");
        if (iterator == client.end() || !iterator->is_object()) return 0;
        return json_intish(*iterator, "id", 0);
    }

    static std::optional<double> array_number(const json& array, std::size_t index) {
        if (!array.is_array() || index >= array.size()) return std::nullopt;
        const auto& value = array[index];
        if (!value.is_number()) return std::nullopt;
        return value.get<double>();
    }

    static std::optional<RawRect> client_rect(const json& client) {
        const auto at = client.find("at");
        const auto size = client.find("size");
        if (at == client.end() || size == client.end()) return std::nullopt;

        const auto x = array_number(*at, 0);
        const auto y = array_number(*at, 1);
        const auto width = array_number(*size, 0);
        const auto height = array_number(*size, 1);
        if (!x || !y || !width || !height || *width <= 1.0 || *height <= 1.0) {
            return std::nullopt;
        }
        return RawRect{*x, *y, *width, *height};
    }

    static std::optional<SelectionRect> clip_to_monitor(
        double x,
        double y,
        double width,
        double height,
        double monitor_width,
        double monitor_height
    ) {
        if (width <= 1.0 || height <= 1.0 ||
            monitor_width <= 1.0 || monitor_height <= 1.0) {
            return std::nullopt;
        }

        const double left = std::clamp(x, 0.0, monitor_width);
        const double top = std::clamp(y, 0.0, monitor_height);
        const double right = std::clamp(x + width, 0.0, monitor_width);
        const double bottom = std::clamp(y + height, 0.0, monitor_height);
        if (right - left <= 2.0 || bottom - top <= 2.0) return std::nullopt;

        return SelectionRect{
            .x = left,
            .y = top,
            .width = right - left,
            .height = bottom - top,
        };
    }

    static const json* layer_levels_for_monitor(
        const json& layers,
        std::string_view connector
    ) {
        if (!layers.is_object()) return nullptr;
        const auto monitor = layers.find(std::string(connector));
        if (monitor == layers.end() || !monitor->is_object()) return nullptr;

        const auto direct = monitor->find("levels");
        if (direct != monitor->end() && direct->is_object()) return &*direct;

        for (auto iterator = monitor->begin(); iterator != monitor->end(); ++iterator) {
            if (!iterator.value().is_object()) continue;
            if (iterator.value().contains("2") || iterator.value().contains("3")) {
                return &iterator.value();
            }
        }
        return nullptr;
    }

    static void append_layer_level(
        SemanticRegionSnapshot& snapshot,
        const MonitorTarget& monitor,
        const json& levels,
        const char* level_key,
        int priority
    ) {
        const auto level = levels.find(level_key);
        if (level == levels.end() || !level->is_array()) return;

        const double monitor_area = monitor.logical_width * monitor.logical_height;
        for (const auto& layer : *level) {
            if (!layer.is_object()) continue;

            const double x = layer.value("x", 0.0) - monitor.layout_x;
            const double y = layer.value("y", 0.0) - monitor.layout_y;
            const double width = layer.value("w", 0.0);
            const double height = layer.value("h", 0.0);
            const auto rect = clip_to_monitor(
                x,
                y,
                width,
                height,
                monitor.logical_width,
                monitor.logical_height
            );
            if (!rect) continue;

            const double area = rect->width * rect->height;
            // Large transparent shell/layer surfaces are terrible hit targets and
            // can swallow every click. Keep semantic layers intentionally local.
            if (monitor_area > 0.0 && area / monitor_area > 0.80) continue;

            std::string name = layer.value("namespace", std::string{});
            if (name == "selection" || name == "realmheart-screenshot") continue;
            if (name.empty()) name = "Layer surface";

            snapshot.regions.push_back(SemanticRegion{
                .rect = *rect,
                .source = SemanticRegionSource::Layer,
                .label = std::move(name),
                .priority = priority,
                .focus_history_id = -1,
            });
        }
    }

    static void append_layer_regions(
        SemanticRegionSnapshot& snapshot,
        const MonitorTarget& monitor,
        const json& layers
    ) {
        const json* levels = layer_levels_for_monitor(layers, monitor.connector);
        if (levels == nullptr) return;

        // Overlay/top layers are the only layer-shell surfaces that can be visibly
        // above client content. Huge/global surfaces are rejected above.
        append_layer_level(snapshot, monitor, *levels, "3", 8);
        append_layer_level(snapshot, monitor, *levels, "2", 12);
    }
};

inline const char* semantic_region_source_label(SemanticRegionSource source) {
    switch (source) {
        case SemanticRegionSource::Content:
            return "Content";
        case SemanticRegionSource::Layer:
            return "Layer";
        case SemanticRegionSource::Window:
        default:
            return "Window";
    }
}

} // namespace realmheart::screenshot
