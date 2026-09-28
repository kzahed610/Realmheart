#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace realmheart::ui::workspace::animation {

// Convert GdkTexture::download()'s Cairo ARGB32 byte order to GL_RGBA while
// checking alpha in the same pass. Cairo's byte order depends on the host.
[[nodiscard]] inline bool convert_cairo_argb32_to_rgba_and_find_alpha(
    std::span<std::uint8_t> pixels,
    bool little_endian
) noexcept {
    bool has_nontransparent_alpha = false;
    for (std::size_t offset = 0; offset + 3U < pixels.size(); offset += 4U) {
        if (little_endian) {
            has_nontransparent_alpha |= pixels[offset + 3U] != 0U;
            std::swap(pixels[offset], pixels[offset + 2U]);
        } else {
            const std::uint8_t alpha = pixels[offset];
            const std::uint8_t red = pixels[offset + 1U];
            const std::uint8_t green = pixels[offset + 2U];
            const std::uint8_t blue = pixels[offset + 3U];
            has_nontransparent_alpha |= alpha != 0U;
            pixels[offset] = red;
            pixels[offset + 1U] = green;
            pixels[offset + 2U] = blue;
            pixels[offset + 3U] = alpha;
        }
    }
    return has_nontransparent_alpha;
}

} // namespace realmheart::ui::workspace::animation
