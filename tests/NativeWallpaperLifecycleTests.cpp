#include "wallpaper-native/NativeWallpaperContracts.hpp"

#include <gtest/gtest.h>

namespace {

using realmheart::wallpaper_native::NativeRecoveryState;
using realmheart::wallpaper_native::NativeRestartDecision;
using realmheart::wallpaper_native::native_command_ready;
using realmheart::wallpaper_native::native_output_requires_redecode;
using realmheart::wallpaper_native::native_output_should_recreate;
using realmheart::wallpaper_native::native_restart_decision;

TEST(NativeWallpaperLifecycleTest, RecoveryIsBoundedAndRequiresReplayState) {
    EXPECT_EQ(
        native_restart_decision({false, true, 0}),
        NativeRestartDecision::RestartAndReplay
    );
    EXPECT_EQ(
        native_restart_decision({false, true, 1}),
        NativeRestartDecision::FailClosed
    );
    EXPECT_EQ(
        native_restart_decision({false, false, 0}),
        NativeRestartDecision::FailClosed
    );
    EXPECT_EQ(
        native_restart_decision({true, true, 0}),
        NativeRestartDecision::Suppress
    );
}

TEST(NativeWallpaperLifecycleTest, CommandCannotSucceedWithoutEveryReadinessBoundary) {
    EXPECT_TRUE(native_command_ready(true, true, true));
    EXPECT_FALSE(native_command_ready(false, true, true));
    EXPECT_FALSE(native_command_ready(true, false, true));
    EXPECT_FALSE(native_command_ready(true, true, false));
}

TEST(NativeWallpaperLifecycleTest, ClosedOutputOnlyRecreatesWhenWaylandOutputRemains) {
    EXPECT_TRUE(native_output_should_recreate(true, true));
    EXPECT_FALSE(native_output_should_recreate(false, true));
    EXPECT_FALSE(native_output_should_recreate(true, false));
}

TEST(NativeWallpaperLifecycleTest, TopologyRedeocodeOnlyRunsForLargerRequiredPixels) {
    EXPECT_TRUE(native_output_requires_redecode(1920, 1080, 3840, 2160));
    EXPECT_TRUE(native_output_requires_redecode(3840, 2160, 3840, 2161));
    EXPECT_FALSE(native_output_requires_redecode(3840, 2160, 2560, 1440));
}

} // namespace
