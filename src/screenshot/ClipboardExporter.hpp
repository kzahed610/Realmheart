#pragma once

#include "screenshot/SelectionGeometry.hpp"
#include "screenshot/WaylandScreencopy.hpp"

#include <string>

namespace realmheart::screenshot {

class ClipboardExporter {
public:
    static bool copy_png(
        const FrozenFrame& frame,
        const PixelRect& region,
        std::string& error
    );

    static bool copy_text(
        const std::string& text,
        std::string& error
    );
};

} // namespace realmheart::screenshot
