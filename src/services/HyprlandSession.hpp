#pragma once

#include "core/Command.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::services {

struct HyprlandSessionWindow {
    std::string address;
    std::string app_id;
    std::string title;
    int workspace_id = 0;
    int focus_history_id = std::numeric_limits<int>::max();
    bool active = false;
};

struct HyprlandSessionSnapshot {
    bool available = false;
    std::string error;
    std::vector<HyprlandSessionWindow> windows;
};

class HyprlandSession {
public:
    static HyprlandSessionSnapshot read(
        const realmheart::core::CommandOptions& options = {}
    );
    static HyprlandSessionSnapshot parse(
        std::string_view clients_json,
        std::string_view active_window_json = "{}"
    );
    static bool focus_window(
        std::string_view address,
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
