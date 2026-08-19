#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace realmheart::services {

// Deterministic layout engine for the Prophecy lock screen.
// Given a seed and a list of futures, produces exact shard geometry
// (positions, sizes, UV coordinates) for the renderer.
// Output is fully deterministic: same seed + same futures = same layout.
class ProphecyLayoutEngine {
public:
    struct FutureGeometry {
        int workspace_id = 0;
        bool is_active = false;
        bool is_dominant = false;
        // Normalized [0,1] coordinates relative to the canvas.
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
        // UV coordinates within the source snapshot texture.
        double uv_x = 0.0;
        double uv_y = 0.0;
        double uv_w = 1.0;
        double uv_h = 1.0;
    };

    struct Layout {
        std::uint64_t seed = 0;
        int canvas_width = 1920;
        int canvas_height = 1080;
        std::vector<FutureGeometry> futures;
        // Rinia anchor placement (normalized coordinates).
        double rinia_x = 0.0;
        double rinia_y = 0.0;
        double rinia_width = 0.0;
        double rinia_height = 0.0;
        // Protected regions (where no shards render).
        struct ProtectedRegion {
            double x = 0.0;
            double y = 0.0;
            double width = 0.0;
            double height = 0.0;
        };
        std::array<ProtectedRegion, 3> protected_regions;
    };

    // Compute a full layout from a seed and future count.
    // futures_count must be between 4 and 6.
    // Returns empty layout if futures_count is out of range.
    static Layout compute(
        std::uint64_t seed,
        std::size_t futures_count,
        int canvas_width = 1920,
        int canvas_height = 1080
    );

private:
    // Splitmix64 PRNG for deterministic pseudo-random values.
    static std::uint64_t splitmix64(std::uint64_t& state);
    static double next_double(std::uint64_t& state);

    // Slot definition: {x, y, w, h} in normalized coordinates.
    using Slot = std::array<double, 4>;

    // Grid presets for 4, 5, and 6 futures.
    // Each defines slots for non-dominant futures plus Rinia anchor position.
    struct GridPreset {
        std::vector<Slot> slots;
        double rinia_x, rinia_y, rinia_w, rinia_h;
    };

    static const GridPreset& preset_for_count(std::size_t count);
};

} // namespace realmheart::services
