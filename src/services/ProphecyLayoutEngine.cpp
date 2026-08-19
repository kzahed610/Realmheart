#include "services/ProphecyLayoutEngine.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>

namespace realmheart::services {

// Splitmix64 — deterministic PRNG from https://prng.di.unimi.it/splitmix64.c
std::uint64_t ProphecyLayoutEngine::splitmix64(std::uint64_t& state) {
    std::uint64_t result = state += 0x9E3779B97F4A7C15ULL;
    result = (result ^ (result >> 30)) * 0xBF58476D1CE4E5B9ULL;
    result = (result ^ (result >> 27)) * 0x94D049BB133111EBULL;
    result = result ^ (result >> 31);
    return result;
}

double ProphecyLayoutEngine::next_double(std::uint64_t& state) {
    return static_cast<double>(splitmix64(state)) / static_cast<double>(UINT64_MAX);
}

// Grid presets defined as static data (avoids constexpr std::vector issues).
const ProphecyLayoutEngine::GridPreset& ProphecyLayoutEngine::preset_for_count(std::size_t count) {
    static const GridPreset presets[3] = {
        {
            // 4 futures
            {
                {0.08, 0.15, 0.28, 0.30},   // top-left
                {0.64, 0.15, 0.28, 0.30},   // top-right
                {0.08, 0.55, 0.28, 0.30},   // bottom-left
                {0.64, 0.55, 0.28, 0.30},   // bottom-right
            },
            0.38, 0.48, 0.24, 0.30,  // Rinia anchor: center-bottom
        },
        {
            // 5 futures
            {
                {0.05, 0.10, 0.26, 0.28},   // top-left
                {0.37, 0.10, 0.26, 0.28},   // top-center
                {0.67, 0.10, 0.26, 0.28},   // top-right
                {0.05, 0.55, 0.26, 0.28},   // bottom-left
                {0.67, 0.55, 0.26, 0.28},   // bottom-right
            },
            0.37, 0.47, 0.26, 0.28,
        },
        {
            // 6 futures
            {
                {0.05, 0.08, 0.24, 0.26},   // top-left
                {0.37, 0.08, 0.24, 0.26},   // top-center
                {0.69, 0.08, 0.24, 0.26},   // top-right
                {0.05, 0.55, 0.24, 0.26},   // bottom-left
                {0.37, 0.55, 0.24, 0.26},   // bottom-center
                {0.69, 0.55, 0.24, 0.26},   // bottom-right
            },
            0.37, 0.47, 0.24, 0.26,
        },
    };
    // count is 4, 5, or 6 → index 0, 1, or 2
    const std::size_t index = count - 4;
    return presets[index];
}

ProphecyLayoutEngine::Layout
ProphecyLayoutEngine::compute(
    std::uint64_t seed,
    std::size_t futures_count,
    int canvas_width,
    int canvas_height
) {
    Layout layout;
    layout.seed = seed;
    layout.canvas_width = canvas_width;
    layout.canvas_height = canvas_height;

    if (futures_count < 4 || futures_count > 6) return layout;  // empty futures

    const GridPreset& preset = preset_for_count(futures_count);

    // The dominant future (active workspace) goes in the center slot.
    std::uint64_t rng = seed;

    FutureGeometry dominant;
    dominant.is_dominant = true;
    dominant.is_active = true;
    // Dominant future occupies the center area.
    dominant.x = 0.34;
    dominant.y = 0.40;
    dominant.width = 0.32;
    dominant.height = 0.28;

    layout.futures.push_back(dominant);

    // Place non-dominant futures in preset slots with light deterministic jitter.
    for (std::size_t i = 0; i < futures_count - 1; ++i) {
        const Slot& slot = preset.slots[i];
        FutureGeometry fg;
        fg.is_dominant = false;
        fg.is_active = false;

        // Apply ±3% jitter to position for variety, but keep within bounds.
        const double jitter_x = (next_double(rng) - 0.5) * 0.03;
        const double jitter_y = (next_double(rng) - 0.5) * 0.03;

        fg.x = std::clamp(slot[0] + jitter_x, 0.02, 0.98 - slot[2]);
        fg.y = std::clamp(slot[1] + jitter_y, 0.02, 0.98 - slot[3]);
        fg.width = slot[2];
        fg.height = slot[3];

        layout.futures.push_back(fg);
    }

    // Rinia anchor placement.
    layout.rinia_x = preset.rinia_x;
    layout.rinia_y = preset.rinia_y;
    layout.rinia_width = preset.rinia_w;
    layout.rinia_height = preset.rinia_h;

    // Protected regions: areas where shards must not render.
    // 1. Rinia area (the character must never be occluded).
    layout.protected_regions[0] = {
        preset.rinia_x - 0.02, preset.rinia_y - 0.02,
        preset.rinia_w + 0.04, preset.rinia_h + 0.04
    };
    // 2. Top-center: clock/date region.
    layout.protected_regions[1] = {
        0.35, 0.02,
        0.30, 0.10
    };
    // 3. Bottom-center: password entry region.
    layout.protected_regions[2] = {
        0.35, 0.83,
        0.30, 0.12
    };

    return layout;
}

} // namespace realmheart::services
