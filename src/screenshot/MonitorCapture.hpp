#pragma once

#include "screenshot/MonitorResolver.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace realmheart::screenshot {

struct CaptureResult {
    std::optional<std::filesystem::path> path;
    std::string error;
};

class MonitorCapture {
public:
    static CaptureResult capture_once(const MonitorTarget& monitor);
    static void remove_quietly(const std::filesystem::path& path);
};

} // namespace realmheart::screenshot
