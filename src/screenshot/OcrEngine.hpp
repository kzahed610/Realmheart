#pragma once

#include "screenshot/SelectionGeometry.hpp"
#include "screenshot/WaylandScreencopy.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace realmheart::screenshot {

struct OcrWord {
    PixelRect rect;
    std::string text;
    float confidence = -1.0f;
    int block = 0;
    int paragraph = 0;
    int line = 0;
    int word = 0;
};

struct OcrResult {
    bool ok = false;
    std::vector<OcrWord> words;
    std::string error;
};

class OcrEngine {
public:
    static bool available(std::string& error);

    static OcrResult recognize(
        const FrozenFrame& frame,
        const PixelRect& region,
        const std::atomic_bool* cancel_requested = nullptr
    );
};

} // namespace realmheart::screenshot
