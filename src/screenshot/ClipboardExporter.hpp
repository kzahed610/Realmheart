#pragma once

#include "screenshot/SelectionGeometry.hpp"
#include "screenshot/WaylandScreencopy.hpp"

#include <atomic>
#include <string>

namespace realmheart::screenshot {

class ClipboardExporter {
public:
    static bool copy_png(
        const FrozenFrame& frame,
        const PixelRect& region,
        std::string& error,
        const std::atomic_bool* cancel_requested = nullptr
    );

    static bool copy_text(
        const std::string& text,
        std::string& error,
        const std::atomic_bool* cancel_requested = nullptr
    );
};

} // namespace realmheart::screenshot
