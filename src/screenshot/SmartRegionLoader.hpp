#pragma once

#include "screenshot/MonitorResolver.hpp"
#include "screenshot/SemanticRegions.hpp"
#include "screenshot/WaylandScreencopy.hpp"

#include <atomic>
#include <string>

namespace realmheart::screenshot {

struct SmartRegionLoadResult {
    SemanticRegionSnapshot snapshot;
    double semantic_ms = 0.0;
    double content_ms = 0.0;
    std::string semantic_error;
    std::string content_error;
};

class SmartRegionLoader {
public:
    // Blocking by design: ScreenshotOverlay runs this on a worker thread only.
    static SmartRegionLoadResult load(
        const MonitorTarget& monitor,
        const FrozenFrame& frame,
        const std::atomic_bool* cancel_requested = nullptr
    );
};

} // namespace realmheart::screenshot
