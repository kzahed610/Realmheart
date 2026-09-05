#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace realmheart::screenshot {

struct MonitorTarget {
    std::string connector;
    int id = -1;
    int physical_width = 0;
    int physical_height = 0;
    double scale = 1.0;
    int transform = 0;
    double layout_x = 0.0;
    double layout_y = 0.0;
    double logical_width = 0.0;
    double logical_height = 0.0;
    int active_workspace_id = 0;
    int special_workspace_id = 0;
};

struct MonitorResolveResult {
    std::optional<MonitorTarget> monitor;
    std::string error;
};

class MonitorResolver {
public:
    static MonitorResolveResult from_hyprland_json(
        std::string_view cursor_json,
        std::string_view monitors_json
    );

    static MonitorResolveResult detect_under_cursor();
};

} // namespace realmheart::screenshot
