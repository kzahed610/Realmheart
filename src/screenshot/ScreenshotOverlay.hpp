#pragma once

#include "screenshot/MonitorResolver.hpp"
#include "screenshot/WaylandScreencopy.hpp"

#include <chrono>

namespace realmheart::screenshot {

class ScreenshotOverlay {
public:
    static int run(
        const FrozenFrame& frozen_frame,
        const MonitorTarget& monitor,
        std::chrono::steady_clock::time_point process_start
    );
};

} // namespace realmheart::screenshot
