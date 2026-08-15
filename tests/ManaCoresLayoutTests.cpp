// tests/ManaCoresLayoutTests.cpp
#include <gtest/gtest.h>
#include "relictombs/ManaCoresLayout.hpp"

TEST(ManaCoresLayout, CoreIsOnLeftSide) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    // Core should be on left side (~50px from left edge at 1080p)
    EXPECT_NEAR(l.core_centre_x, 50.0, 1.0);
}

TEST(ManaCoresLayout, CoreIsCentredVertically) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    EXPECT_NEAR(l.core_centre_y, l.canvas_height / 2.0, 1.0);
}

TEST(ManaCoresLayout, RadialsParkedRightOfCore) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    EXPECT_GT(l.radials_park_x, l.core_centre_x + l.core_radius);
}

TEST(ManaCoresLayout, RadialsSeparatedByTenPx) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    EXPECT_NEAR(l.radial_spacing, 10.0, 0.5);
}

TEST(ManaCoresLayout, RadialPaletteHasThreeColours) {
    auto l = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    EXPECT_EQ(l.kRadialPalette.size(), 3);
}

TEST(ManaCoresLayout, ScalesWithHeight) {
    auto l1080 = realmheart::relictombs::ManaCoresLayout::for_height(1080);
    auto l1440 = realmheart::relictombs::ManaCoresLayout::for_height(1440);

    EXPECT_NEAR(l1440.core_radius / l1080.core_radius, 1440.0 / 1080.0, 0.01);
    EXPECT_NEAR(l1440.radial_radius / l1080.radial_radius, 1440.0 / 1080.0, 0.01);
    EXPECT_NEAR(l1440.radial_spacing / l1080.radial_spacing, 1440.0 / 1080.0, 0.01);
}