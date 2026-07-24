#include "animation/character/HairFlowWarp.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace realmheart::animation::character {
namespace {

struct Pixel {
    double a = 0.0;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

bool valid_view(const Argb32ImageView& view) {
    return view.data != nullptr && view.width > 0 && view.height > 0 &&
        view.stride >= view.width * 4;
}

std::uint32_t load_word(const Argb32ImageView& view, int x, int y) {
    std::uint32_t word = 0;
    std::memcpy(
        &word,
        view.data + (static_cast<std::size_t>(y) * view.stride) +
            (static_cast<std::size_t>(x) * 4U),
        sizeof(word)
    );
    return word;
}

Pixel unpack(std::uint32_t word) {
    return {
        .a = static_cast<double>((word >> 24U) & 0xffU),
        .r = static_cast<double>((word >> 16U) & 0xffU),
        .g = static_cast<double>((word >> 8U) & 0xffU),
        .b = static_cast<double>(word & 0xffU),
    };
}

std::uint32_t pack(Pixel pixel) {
    const auto channel = [](double value) {
        return static_cast<std::uint32_t>(std::clamp(
            std::lround(value),
            0L,
            255L
        ));
    };
    const std::uint32_t a = channel(pixel.a);
    const std::uint32_t r = std::min(channel(pixel.r), a);
    const std::uint32_t g = std::min(channel(pixel.g), a);
    const std::uint32_t b = std::min(channel(pixel.b), a);
    return (a << 24U) | (r << 16U) | (g << 8U) | b;
}

Pixel sample_bilinear(const Argb32ImageView& source, double x, double y) {
    if (x < -1.0 || y < -1.0 || x > source.width || y > source.height) {
        return {};
    }

    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const double fx = x - static_cast<double>(x0);
    const double fy = y - static_cast<double>(y0);

    const auto read = [&source](int px, int py) {
        if (px < 0 || py < 0 || px >= source.width || py >= source.height) {
            return Pixel{};
        }
        return unpack(load_word(source, px, py));
    };

    const Pixel p00 = read(x0, y0);
    const Pixel p10 = read(x1, y0);
    const Pixel p01 = read(x0, y1);
    const Pixel p11 = read(x1, y1);
    const auto mix = [fx, fy](double a, double b, double c, double d) {
        const double top = a + ((b - a) * fx);
        const double bottom = c + ((d - c) * fx);
        return top + ((bottom - top) * fy);
    };

    return {
        .a = mix(p00.a, p10.a, p01.a, p11.a),
        .r = mix(p00.r, p10.r, p01.r, p11.r),
        .g = mix(p00.g, p10.g, p01.g, p11.g),
        .b = mix(p00.b, p10.b, p01.b, p11.b),
    };
}

std::pair<double, double> flow_vector(
    const Argb32ImageView& flow,
    int x,
    int y
) {
    const Pixel encoded = unpack(load_word(flow, x, y));
    if (encoded.a <= 0.5) return {0.0, 0.0};

    const double unpremultiply = 255.0 / encoded.a;
    const double red = std::clamp(encoded.r * unpremultiply, 0.0, 255.0);
    const double green = std::clamp(encoded.g * unpremultiply, 0.0, 255.0);
    return {
        std::clamp((red - 128.0) / 127.0, -1.0, 1.0),
        std::clamp((green - 128.0) / 127.0, -1.0, 1.0),
    };
}

double movement_weight(const Argb32ImageView& mask, int x, int y) {
    const Pixel pixel = unpack(load_word(mask, x, y));
    // Cairo stores premultiplied channels. Their average divided by 255 is
    // exactly the authored grayscale value multiplied by alpha, matching the
    // mask contract used by HairMesh.
    return std::clamp((pixel.r + pixel.g + pixel.b) / (3.0 * 255.0), 0.0, 1.0);
}

} // namespace

std::optional<std::vector<std::uint8_t>> warp_hair_argb32(
    Argb32ImageView source,
    Argb32ImageView flow,
    Argb32ImageView movement_mask,
    double displacement_pixels,
    std::string* error_message
) {
    const auto fail = [error_message](const char* message) {
        if (error_message != nullptr) *error_message = message;
        return std::optional<std::vector<std::uint8_t>>{};
    };

    if (!valid_view(source) || !valid_view(flow) || !valid_view(movement_mask)) {
        return fail("Hair flow warp received an invalid ARGB32 image view");
    }
    if (source.width != flow.width || source.height != flow.height ||
        source.width != movement_mask.width ||
        source.height != movement_mask.height) {
        return fail("Hair, flow map, and movement mask dimensions must match");
    }
    if (!std::isfinite(displacement_pixels) ||
        std::abs(displacement_pixels) > 16.0) {
        return fail("Hair flow displacement is outside the accepted range");
    }

    const int output_stride = source.width * 4;
    const std::size_t byte_count = static_cast<std::size_t>(output_stride) *
        static_cast<std::size_t>(source.height);
    if (byte_count > std::numeric_limits<std::size_t>::max() / 2U) {
        return fail("Hair flow output size overflowed");
    }
    std::vector<std::uint8_t> output(byte_count, 0U);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const auto [direction_x, direction_y] = flow_vector(flow, x, y);
            const double strength = movement_weight(movement_mask, x, y);
            const double source_x = static_cast<double>(x) -
                (direction_x * strength * displacement_pixels);
            const double source_y = static_cast<double>(y) -
                (direction_y * strength * displacement_pixels);
            const std::uint32_t word = pack(sample_bilinear(source, source_x, source_y));
            std::memcpy(
                output.data() + (static_cast<std::size_t>(y) * output_stride) +
                    (static_cast<std::size_t>(x) * 4U),
                &word,
                sizeof(word)
            );
        }
    }

    return output;
}

} // namespace realmheart::animation::character
