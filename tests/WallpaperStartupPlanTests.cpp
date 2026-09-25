#include "ui/wallpaper/WallpaperStartupPlan.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <vector>

namespace {

using namespace realmheart::ui::wallpaper;

TEST(WallpaperStartupPlanTest, OutputOverrideWinsAndGlobalSourceOnlyFillsMissingOutputs) {
    const std::vector<WallpaperStartupOutput> outputs{
        {
            WallpaperOutputTarget{0, "eDP-1"},
            WallpaperSource{std::filesystem::path{"/wallpapers/current-output.png"}}
        },
        {
            WallpaperOutputTarget{1, "HDMI-A-1"},
            std::nullopt
        }
    };
    const std::optional<WallpaperSource> global_source{
        WallpaperSource{std::filesystem::path{"/wallpapers/stale-global.png"}}
    };

    const auto plan = make_startup_wallpaper_plan(outputs, global_source);

    EXPECT_TRUE(plan.has_output_overrides);
    ASSERT_EQ(plan.jobs.size(), 2U);
    EXPECT_EQ(plan.jobs[0].target.connector, "eDP-1");
    EXPECT_EQ(plan.jobs[0].source.path().string(), "/wallpapers/current-output.png");
    EXPECT_EQ(plan.jobs[1].target.connector, "HDMI-A-1");
    EXPECT_EQ(plan.jobs[1].source.path().string(), "/wallpapers/stale-global.png");
}

TEST(WallpaperStartupPlanTest, GlobalOnlyRestorationDoesNotEnterOutputOverrideMode) {
    const std::vector<WallpaperStartupOutput> outputs{
        {WallpaperOutputTarget{0, "eDP-1"}, std::nullopt}
    };
    const std::optional<WallpaperSource> global_source{
        WallpaperSource{std::filesystem::path{"/wallpapers/global.png"}}
    };

    const auto plan = make_startup_wallpaper_plan(outputs, global_source);

    EXPECT_FALSE(plan.has_output_overrides);
    ASSERT_EQ(plan.jobs.size(), 1U);
    EXPECT_EQ(plan.jobs[0].source.path().string(), "/wallpapers/global.png");
}

TEST(WallpaperStartupPlanTest, OmitsOutputsWithoutAnySavedSource) {
    const std::vector<WallpaperStartupOutput> outputs{
        {WallpaperOutputTarget{0, "eDP-1"}, std::nullopt},
        {WallpaperOutputTarget{-1, ""}, WallpaperSource{
            std::filesystem::path{"/wallpapers/unroutable.png"}
        }}
    };

    const auto plan = make_startup_wallpaper_plan(outputs, std::nullopt);

    EXPECT_FALSE(plan.has_output_overrides);
    EXPECT_TRUE(plan.jobs.empty());
}

} // namespace
