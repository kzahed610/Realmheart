#include "wallpaper-native/gpu/YuvColorimetry.hpp"

#include <algorithm>

namespace realmheart::wallpaper_native {

YuvConversion yuv_conversion_for(
    VideoColorSpace color_space,
    VideoColorRange color_range
) noexcept {
    YuvConversion conversion;

    // Limited-range luma sits in 16..235, so it is lifted and rescaled to fill
    // 0..1 before the matrix applies. Full-range luma already does.
    const float luma_scale = color_range == VideoColorRange::Full
        ? 1.0F
        : 255.0F / 219.0F;
    conversion.luma_offset =
        color_range == VideoColorRange::Full
        ? 0.0F
        : -16.0F / 255.0F;

    // BT.601 (kr 0.299, kb 0.114) and BT.709 (kr 0.2126, kb 0.0722) differ only
    // in the relative weight of the chroma contributions.
    const double kr = color_space == VideoColorSpace::Bt709 ? 0.2126 : 0.299;
    const double kb = color_space == VideoColorSpace::Bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;

    // The ITU-R coefficients, derived from the primaries:
    //   R = y' + 2(1-kr)            * Cr
    //   G = y' - 2(1-kr)kr/kg * Cr - 2(1-kb)kb/kg * Cb
    //   B = y' + 2(1-kb)            * Cb
    //
    // The chroma coefficients deliberately do not carry the limited-range
    // factor: the ITU values already assume lifted luma, and scaling them again
    // tints every mid chroma.
    const double red_from_chroma = 2.0 * (1.0 - kr);
    const double green_from_luma_chroma = -2.0 * (1.0 - kr) * kr / kg;
    const double green_from_blue_chroma = -2.0 * (1.0 - kb) * kb / kg;
    const double blue_from_chroma = 2.0 * (1.0 - kb);

    conversion.matrix[0] = static_cast<float>(luma_scale);
    conversion.matrix[1] = 0.0F;
    conversion.matrix[2] = static_cast<float>(red_from_chroma);

    conversion.matrix[3] = static_cast<float>(luma_scale);
    conversion.matrix[4] = static_cast<float>(green_from_blue_chroma);
    conversion.matrix[5] = static_cast<float>(green_from_luma_chroma);

    conversion.matrix[6] = static_cast<float>(luma_scale);
    conversion.matrix[7] = static_cast<float>(blue_from_chroma);
    conversion.matrix[8] = 0.0F;
    return conversion;
}

} // namespace realmheart::wallpaper_native
