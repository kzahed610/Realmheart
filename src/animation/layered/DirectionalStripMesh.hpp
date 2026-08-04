#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace realmheart::animation::layered {

enum class StripAxis { Rows, Columns };
enum class AnchorPolicy { PinnedMinimum, PinnedMaximum, WeightedTranslate };

struct Vector2 {
    double x = 0.0;
    double y = 0.0;
};

struct DirectionalStrip {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double weight = 0.0;
    double visible_min = 0.0;
    double visible_max = 0.0;
};

struct DirectionalStripDeformation {
    Vector2 minimum_offset;
    Vector2 maximum_offset;
};

class DirectionalStripMesh {
public:
    static std::optional<DirectionalStripMesh> from_argb32(
        const std::uint8_t* data,
        int width,
        int height,
        int stride,
        int requested_strips,
        StripAxis axis,
        AnchorPolicy anchor,
        Vector2 direction,
        std::string* error_message = nullptr
    );

    [[nodiscard]] const std::vector<DirectionalStrip>& strips() const;
    [[nodiscard]] std::vector<DirectionalStripDeformation> pose(
        double displacement
    ) const;
    [[nodiscard]] StripAxis axis() const;
    [[nodiscard]] AnchorPolicy anchor() const;
    [[nodiscard]] Vector2 direction() const;
    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;

private:
    int width_ = 0;
    int height_ = 0;
    StripAxis axis_ = StripAxis::Rows;
    AnchorPolicy anchor_ = AnchorPolicy::WeightedTranslate;
    Vector2 direction_;
    std::vector<DirectionalStrip> strips_;
};

} // namespace realmheart::animation::layered
