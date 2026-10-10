#include "wallpaper-native/source/StaticDecodePlan.hpp"

#include <gtest/gtest.h>

using realmheart::wallpaper_native::kMaxStaticDecodedPixels;
using realmheart::wallpaper_native::plan_static_decode;
using realmheart::wallpaper_native::StaticDecodePlan;

// Phase 0 regression: decode geometry. These cases pin the exact sizing rules
// the static source must keep after the source-abstraction refactor.
//
// Note: decode geometry is deliberately source-native. The legacy renderer's
// per-output "required_scale" loop could only ever grow a scale above 1.0, so
// output topology never shrank a decode. These tests exist so that a later
// "optimization" that starts decoding down to output size is a visible,
// reviewed behavior change instead of a silent memory/quality regression.

TEST(StaticDecodePlanTest, DecodesAtSourceResolution) {
    const auto plan = plan_static_decode(3840, 2160, 16384);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.target_width, 3840);
    EXPECT_EQ(plan.target_height, 2160);
    EXPECT_FALSE(plan.downscaled);
}

TEST(StaticDecodePlanTest, ClampsToTheMaximumTextureSize) {
    const auto plan = plan_static_decode(8192, 4096, 4096);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.target_width, 4096);
    EXPECT_EQ(plan.target_height, 2048);
    EXPECT_TRUE(plan.downscaled);
}

TEST(StaticDecodePlanTest, ClampKeepsTheSourceAspectRatio) {
    const auto plan = plan_static_decode(6000, 3000, 2000);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.target_width, 2000);
    EXPECT_EQ(plan.target_height, 1000);
}

TEST(StaticDecodePlanTest, UnknownTextureLimitIsNotApplied) {
    const auto plan = plan_static_decode(8192, 4096, 0);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.target_width, 8192);
    EXPECT_EQ(plan.target_height, 4096);
    EXPECT_FALSE(plan.downscaled);
}

TEST(StaticDecodePlanTest, NegativeTextureLimitIsIgnored) {
    const auto plan = plan_static_decode(2048, 1024, -1);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.target_width, 2048);
    EXPECT_EQ(plan.target_height, 1024);
    EXPECT_FALSE(plan.downscaled);
}

TEST(StaticDecodePlanTest, SourceExactlyAtTheTextureLimitIsNotScaled) {
    const auto plan = plan_static_decode(4096, 4096, 4096);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.target_width, 4096);
    EXPECT_EQ(plan.target_height, 4096);
    EXPECT_FALSE(plan.downscaled);
}

TEST(StaticDecodePlanTest, AlwaysLeavesAtLeastOnePixel) {
    // 10000x6000 stays inside the pixel budget but is far beyond a 1 px GL
    // texture limit, so the clamp must floor to a single pixel, not to zero.
    const auto plan = plan_static_decode(10000, 6000, 1);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.target_width, 1);
    EXPECT_EQ(plan.target_height, 1);
}

TEST(StaticDecodePlanTest, RejectsSourcesBeyondTheDecodeBudget) {
    const int too_wide = static_cast<int>(kMaxStaticDecodedPixels) + 1;
    EXPECT_FALSE(plan_static_decode(too_wide, 1, 16384).valid);
    EXPECT_FALSE(plan_static_decode(20000, 20000, 16384).valid);
    EXPECT_TRUE(plan_static_decode(8000, 8000, 16384).valid);
}

TEST(StaticDecodePlanTest, RejectsNonPositiveDimensions) {
    EXPECT_FALSE(plan_static_decode(0, 1080, 16384).valid);
    EXPECT_FALSE(plan_static_decode(1920, 0, 16384).valid);
    EXPECT_FALSE(plan_static_decode(-1, 1080, 16384).valid);
    EXPECT_FALSE(plan_static_decode(1920, -1080, 16384).valid);
}

TEST(StaticDecodePlanTest, ReportsDecodedGeometryForReuseByCallers) {
    const StaticDecodePlan plan = plan_static_decode(3840, 2400, 16384);
    ASSERT_TRUE(plan.valid);
    EXPECT_EQ(plan.source_width, 3840);
    EXPECT_EQ(plan.source_height, 2400);
    EXPECT_EQ(plan.target_width, 3840);
    EXPECT_EQ(plan.target_height, 2400);
    EXPECT_LE(plan.target_width, plan.source_width);
    EXPECT_LE(plan.target_height, plan.source_height);
}
