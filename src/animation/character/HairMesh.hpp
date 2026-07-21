#pragma once

#include <optional>
#include <string>
#include <vector>

namespace realmheart::animation::character {

struct HairMeshBand {
    int y = 0;
    int height = 0;
    double weight = 0.0;

    // Visible horizontal bounds recovered from the alpha-bearing movement
    // mask. The inner bound is the real hair/body attachment edge; pinning the
    // PNG canvas edge is insufficient when the asset contains transparent
    // padding.
    double outer_x = 0.0;
    double inner_x = 0.0;
};

// Lightweight mask-derived strip mesh used for macro hair inertia. It stores
// only deformation weights; the grayscale mask surface can be released as soon
// as construction finishes.
class HairMesh {
public:
    static std::optional<HairMesh> from_argb32(
        const unsigned char* data,
        int width,
        int height,
        int stride,
        int requested_rows,
        std::string* error_message = nullptr
    );

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<HairMeshBand>& bands() const noexcept {
        return bands_;
    }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<HairMeshBand> bands_;
};

} // namespace realmheart::animation::character
