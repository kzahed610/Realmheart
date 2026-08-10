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

} // namespace
} // namespace realmheart::relictombs
