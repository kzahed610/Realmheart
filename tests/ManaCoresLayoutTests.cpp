// tests/ManaCoresLayoutTests.cpp
#include <gtest/gtest.h>
#include "relictombs/ManaCoresLayout.hpp"

TEST(ManaCoresLayout, CoreIsCentredVertically) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    EXPECT_NEAR(l.core_centre_y, l.canvas_height / 2.0, 1.0);
}

TEST(ManaCoresLayout, CoreIsLeftAligned) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    // Core is placed on the left side ~320px in
    EXPECT_NEAR(l.core_centre_x, 320.0, 1.0);
}

TEST(ManaCoresLayout, ExpandedCoreIsLargerThanSmallCore) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    EXPECT_GT(l.core_radius_expanded, l.core_radius_small);
    EXPECT_GT(l.core_radius_small, l.core_radius_shrunk);
}

TEST(ManaCoresLayout, DetachedRadialSlicesSpanRightHalf) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    // Slice 0 (Silver) should be top-right
    EXPECT_LT(l.detached_slices[0].start_angle, 0.0);
    EXPECT_LT(l.detached_slices[0].end_angle, 0.0);
    // Slice 1 (Yellow) should be centered on 0 (due right)
    EXPECT_NEAR(l.detached_slices[1].mid_angle, 0.0, 0.01);
    // Slice 2 (Orange) should be bottom-right
    EXPECT_GT(l.detached_slices[2].start_angle, 0.0);
    EXPECT_GT(l.detached_slices[2].end_angle, 0.0);
}

TEST(ManaCoresLayout, DetachedSlicesSeparatedByGaps) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    // Gap between slice 0 and slice 1
    EXPECT_LT(l.detached_slices[0].end_angle, l.detached_slices[1].start_angle);
    // Gap between slice 1 and slice 2
    EXPECT_LT(l.detached_slices[1].end_angle, l.detached_slices[2].start_angle);
}

TEST(ManaCoresLayout, AttachedSlicesEncompassCore) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    // Slices should encompass the whole core, taking 120 degrees each.
    EXPECT_NEAR(l.attached_slices[0].end_angle - l.attached_slices[0].start_angle, 120.0 * (std::numbers::pi / 180.0), 0.01);
    EXPECT_NEAR(l.attached_slices[1].end_angle - l.attached_slices[1].start_angle, 120.0 * (std::numbers::pi / 180.0), 0.01);
    EXPECT_NEAR(l.attached_slices[2].end_angle - l.attached_slices[2].start_angle, 120.0 * (std::numbers::pi / 180.0), 0.01);
}

TEST(ManaCoresLayout, RadialPaletteHasThreeColours) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    EXPECT_EQ(l.kRadialPalette.size(), 3);
}

TEST(ManaCoresLayout, ScalesWithHeight) {
    auto l1080 = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    auto l1440 = realmheart::relictombs::ManaCoresLayout::for_height(1440);

    EXPECT_NEAR(l1440.core_radius_expanded / l1080.core_radius_expanded, 1440.0 / 1080.0, 0.01);
    EXPECT_NEAR(l1440.core_radius_small / l1080.core_radius_small, 1440.0 / 1080.0, 0.01);
    EXPECT_NEAR(l1440.slice_gap / l1080.slice_gap, 1440.0 / 1080.0, 0.01);
}