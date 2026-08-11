#include "relictombs/RelictombsLayout.hpp"

#include <gtest/gtest.h>

namespace realmheart::relictombs {
namespace {

TEST(RelictombsLayoutTests, ExactSixteenByNineOutputNeedsNoOffset) {
    const SceneTransform transform = make_scene_transform(1920.0F, 1080.0F);

    EXPECT_FLOAT_EQ(transform.scale, 1.0F);
    EXPECT_FLOAT_EQ(transform.offset_x, 0.0F);
    EXPECT_FLOAT_EQ(transform.offset_y, 0.0F);
}

TEST(RelictombsLayoutTests, UltrawideOutputUsesUniformCoverAndCentresCrop) {
    const SceneTransform transform = make_scene_transform(3440.0F, 1440.0F);

    EXPECT_FLOAT_EQ(transform.scale, 3440.0F / kDesignWidth);
    EXPECT_FLOAT_EQ(transform.offset_x, 0.0F);
    EXPECT_LT(transform.offset_y, 0.0F);
    EXPECT_FLOAT_EQ(
        transform.offset_y,
        (1440.0F - kDesignHeight * transform.scale) * 0.5F
    );
}

TEST(RelictombsLayoutTests, PortalViewportStaysInsideDesignCanvas) {
    EXPECT_GT(kPortalViewport.x, 0.0F);
    EXPECT_GT(kPortalViewport.y, 0.0F);
    EXPECT_GT(kPortalViewport.width, 0.0F);
    EXPECT_GT(kPortalViewport.height, 0.0F);
    EXPECT_LT(kPortalViewport.x + kPortalViewport.width, kDesignWidth);
    EXPECT_LT(kPortalViewport.y + kPortalViewport.height, kDesignHeight);
}

TEST(RelictombsLayoutTests, PhysicalOutputHeightSelectsSmallestSharpAssetTier) {
    EXPECT_EQ(select_asset_tier(1080), AssetTier::P1080);
    EXPECT_EQ(select_asset_tier(1081), AssetTier::P1440);
    EXPECT_EQ(select_asset_tier(1440), AssetTier::P1440);
    EXPECT_EQ(select_asset_tier(1441), AssetTier::P2160);
    EXPECT_EQ(select_asset_tier(2160), AssetTier::P2160);
}

TEST(RelictombsLayoutTests, AssetTierUsesCanonicalTierPaths) {
    EXPECT_EQ(
        base_asset_relative_path(AssetTier::P1080),
        "1080p/base.png"
    );
    EXPECT_EQ(
        base_asset_relative_path(AssetTier::P1440),
        "1440p/base.png"
    );
    EXPECT_EQ(
        base_asset_relative_path(AssetTier::P2160),
        "4k/base.png"
    );
}

TEST(RelictombsLayoutTests, FourBrokenFragmentsDefinedWithDistinctRects) {
    ASSERT_EQ(kFragmentSpecs.size(), 4U);

    for (const auto& fragment : kFragmentSpecs) {
        EXPECT_GT(fragment.idle_rect.width, 0.0F);
        EXPECT_GT(fragment.idle_rect.height, 0.0F);
        EXPECT_GT(fragment.idle_rect.x, 0.0F);
        EXPECT_GT(fragment.idle_rect.y, 0.0F);
        // Sprites rest on the arch surround, fully inside the design canvas.
        EXPECT_LT(fragment.idle_rect.x + fragment.idle_rect.width, kDesignWidth);
        EXPECT_LT(
            fragment.idle_rect.y + fragment.idle_rect.height,
            kDesignHeight
        );
        EXPECT_FALSE(fragment.file.empty());
    }
}

TEST(RelictombsLayoutTests, FragmentIdleRectsNeverPileOnEachOther) {
    // The forge pads each sprite by ~10px of transparency around the painted
    // art, so bounding boxes are allowed to graze (top-right vs middle-right
    // overlap by ~20x15px of pure padding with zero painted collision — pixel
    // audit verifies that). A strict bbox-disjoint assertion would be a false
    // oracle. The real constant-level invariant: overlap area must stay far
    // below what an actual pile-up would produce.
    for (std::size_t i = 0; i < kFragmentSpecs.size(); ++i) {
        for (std::size_t j = i + 1; j < kFragmentSpecs.size(); ++j) {
            const auto& a = kFragmentSpecs[i].idle_rect;
            const auto& b = kFragmentSpecs[j].idle_rect;
            const float overlap_w =
                std::min(a.x + a.width, b.x + b.width) -
                std::max(a.x, b.x);
            const float overlap_h =
                std::min(a.y + a.height, b.y + b.height) -
                std::max(a.y, b.y);
            if (overlap_w <= 0.0F || overlap_h <= 0.0F) continue;
            const float overlap_area = overlap_w * overlap_h;
            const float smaller_area =
                std::min(a.width * a.height, b.width * b.height);
            // Padding grazes stay under ~5% of the smaller sprite. A real
            // collision would bury a meaningful fraction of the sprite.
            EXPECT_LT(overlap_area, smaller_area * 0.05F)
                << kFragmentSpecs[i].name << " piles onto "
                << kFragmentSpecs[j].name
                << " (overlap " << overlap_area << " px2)";
        }
    }
}

TEST(RelictombsLayoutTests, FragmentAssetPathsUseTieredFragmentFolders) {
    const auto& first = kFragmentSpecs.front();
    EXPECT_EQ(
        fragment_asset_relative_path(AssetTier::P1080, first),
        "1080p/fragments/" + std::string(first.file)
    );
    EXPECT_EQ(
        fragment_asset_relative_path(AssetTier::P1440, first),
        "1440p/fragments/" + std::string(first.file)
    );
    EXPECT_EQ(
        fragment_asset_relative_path(AssetTier::P2160, first),
        "4k/fragments/" + std::string(first.file)
    );
}

} // namespace
} // namespace realmheart::relictombs
