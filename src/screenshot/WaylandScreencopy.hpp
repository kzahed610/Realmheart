#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace realmheart::screenshot {

struct FrozenFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> rgba;
};

struct ScreencopyResult {
    bool ok = false;
    FrozenFrame frame;
    std::string error;
};

class WaylandScreencopy {
public:
    static ScreencopyResult capture_output(const std::string& monitor_connector);
};

} // namespace realmheart::screenshot
