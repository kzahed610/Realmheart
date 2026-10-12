#include "services/WallpaperRotationSettings.hpp"

#include <gtest/gtest.h>

#include <glib.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using realmheart::services::WallpaperPool;
using realmheart::services::WallpaperRotationSettings;
using realmheart::services::WallpaperRotationStore;
using realmheart::services::default_live_wallpaper_root;
using realmheart::services::default_static_wallpaper_root;

namespace {

class ScopedState {
public:
    ScopedState() {
        std::string pattern = "/tmp/realmheart-rotation-state-XXXXXX";
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = created;
        // Isolate the state path and the default roots, so the real user's
        // settings are never read or written by a test.
        g_setenv(
            "REALMHEART_WALLPAPER_ROTATION_STATE",
            (root_ / "rotation.json").c_str(),
            TRUE
        );
        g_setenv(
            "REALMHEART_WALLPAPER_STATIC_ROOT",
            (root_ / "static").c_str(),
            TRUE
        );
        g_setenv("REALMHEART_WALLPAPER_LIVE_ROOT", (root_ / "live").c_str(), TRUE);
    }

    ~ScopedState() {
        g_unsetenv("REALMHEART_WALLPAPER_ROTATION_STATE");
        g_unsetenv("REALMHEART_WALLPAPER_STATIC_ROOT");
        g_unsetenv("REALMHEART_WALLPAPER_LIVE_ROOT");
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

} // namespace

// Rotation must not be on by accident: the whole stage is "no behaviour change
// until the user enables it".
TEST(WallpaperRotationSettingsTest, DefaultsLeaveRotationOff) {
    const WallpaperRotationSettings settings;
    EXPECT_FALSE(settings.enabled);
    EXPECT_EQ(settings.interval_minutes, WallpaperRotationSettings::kDefaultIntervalMinutes);
    // Every pool carries weight, so enabling both media kinds is a per-pool
    // split rather than a per-file one.
    EXPECT_TRUE(settings.has_enabled_pool());
    for (const int weight : settings.pool_weights) EXPECT_GT(weight, 0);
    // Every pause rule is on: a wallpaper must not yank itself away while the
    // user is looking at one.
    EXPECT_TRUE(settings.pause_when_selector_open);
    EXPECT_TRUE(settings.pause_when_locked);
    EXPECT_TRUE(settings.pause_when_exclusive_fullscreen);
    EXPECT_TRUE(settings.pause_on_critical_battery);
    EXPECT_TRUE(settings.history.empty());
    EXPECT_TRUE(settings.paused_outputs.empty());
}

TEST(WallpaperRotationSettingsTest, PoolNamesAreStable) {
    EXPECT_EQ(wallpaper_pool_name(WallpaperPool::LocalStatic), "local-static");
    EXPECT_EQ(wallpaper_pool_name(WallpaperPool::LocalLive), "local-live");
    EXPECT_EQ(wallpaper_pool_name(WallpaperPool::OnlineStatic), "online-static");
    EXPECT_EQ(wallpaper_pool_name(WallpaperPool::OnlineLive), "online-live");
}

TEST(WallpaperRotationSettingsTest, AllPoolsDisabledMeansNothingToRotate) {
    WallpaperRotationSettings settings;
    for (int& weight : settings.pool_weights) weight = 0;
    EXPECT_FALSE(settings.has_enabled_pool());
}

// A round trip is the contract: what was saved is what comes back, including
// the things a user set to non-default values.
TEST(WallpaperRotationSettingsTest, RoundTripThroughJson) {
    WallpaperRotationSettings settings;
    settings.enabled = true;
    settings.interval_minutes = 45;
    settings.static_root = "/wallpapers/still";
    settings.live_root = "/wallpapers/moving";
    settings.pool_weights = {70, 30, 0, 0};
    settings.pause_when_selector_open = false;
    settings.pause_on_critical_battery = false;
    settings.paused_outputs = {"eDP-1", "HDMI-A-1"};
    settings.history = {"/wallpapers/a.png", "/wallpapers/b.mp4"};

    const auto restored = WallpaperRotationSettings::from_json(settings.to_json());
    EXPECT_TRUE(restored.enabled);
    EXPECT_EQ(restored.interval_minutes, 45);
    EXPECT_EQ(restored.static_root, "/wallpapers/still");
    EXPECT_EQ(restored.live_root, "/wallpapers/moving");
    EXPECT_EQ(restored.pool_weights[0], 70);
    EXPECT_EQ(restored.pool_weights[1], 30);
    EXPECT_EQ(restored.pool_weights[2], 0);
    EXPECT_EQ(restored.pool_weights[3], 0);
    EXPECT_FALSE(restored.pause_when_selector_open);
    EXPECT_TRUE(restored.pause_when_locked);
    EXPECT_TRUE(restored.pause_when_exclusive_fullscreen);
    EXPECT_FALSE(restored.pause_on_critical_battery);
    EXPECT_EQ(restored.paused_outputs.size(), 2U);
    EXPECT_EQ(restored.history.size(), 2U);
    EXPECT_EQ(restored.history.front(), "/wallpapers/a.png");
    EXPECT_EQ(restored.history.back(), "/wallpapers/b.mp4");
}

// A blank store yields defaults and no error: rotation was never configured.
TEST(WallpaperRotationSettingsTest, MissingStateFileYieldsDefaults) {
    ScopedState state;
    WallpaperRotationStore store;
    EXPECT_EQ(store.path(), state.root() / "rotation.json");

    std::string diagnostic;
    const auto settings = store.load(&diagnostic);
    EXPECT_TRUE(diagnostic.empty());
    EXPECT_FALSE(settings.enabled);
}

// A save/load pair writes the file the user asked for, not a temp file beside
// it, and leaves nothing behind.
TEST(WallpaperRotationSettingsTest, SaveThenLoadRoundTripsThroughDisk) {
    ScopedState state;
    WallpaperRotationStore store;

    WallpaperRotationSettings settings;
    settings.enabled = true;
    settings.interval_minutes = 15;
    settings.static_root = state.root() / "still";
    std::string error;
    ASSERT_TRUE(store.save(settings, &error)) << error;

    EXPECT_TRUE(std::filesystem::exists(store.path()));
    EXPECT_FALSE(std::filesystem::exists(store.path().string() + ".partial"));

    std::string diagnostic;
    const auto restored = store.load(&diagnostic);
    EXPECT_TRUE(diagnostic.empty());
    EXPECT_TRUE(restored.enabled);
    EXPECT_EQ(restored.interval_minutes, 15);
    EXPECT_EQ(restored.static_root, state.root() / "still");
}

// Corrupt state must degrade to defaults with a reason, never crash, and never
// delete the user's file.
TEST(WallpaperRotationSettingsTest, CorruptStateYieldsDefaultsAndKeepsTheFile) {
    ScopedState state;
    WallpaperRotationStore store;
    // The sandbox's directory exists already; creating it must not be an error.
    std::error_code directory_error;
    std::filesystem::create_directories(store.path().parent_path(), directory_error);
    ASSERT_FALSE(directory_error);
    std::ofstream(store.path(), std::ios::binary) << "{ not json at all";

    std::string diagnostic;
    const auto settings = store.load(&diagnostic);
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_FALSE(settings.enabled);
    // The file survives: a rewrite happens deliberately, not as collateral.
    EXPECT_TRUE(std::filesystem::exists(store.path()));
}

// Unknown fields are ignored rather than fatal: a settings file written by a
// newer build must not brick an older one.
TEST(WallpaperRotationSettingsTest, UnknownFieldsAreIgnored) {
    nlohmann::json source = WallpaperRotationSettings{}.to_json();
    source["a_future_field"] = {1, 2, 3};
    source["rotation"]["something_nested"] = true;

    std::string diagnostic;
    const auto restored = WallpaperRotationSettings::from_json(source, &diagnostic);
    EXPECT_TRUE(diagnostic.empty());
    EXPECT_FALSE(restored.enabled);
}

// Out-of-range values are clamped, because a hand-edited settings file should
// not disable rotation or wedge the interval.
TEST(WallpaperRotationSettingsTest, NonsenseValuesAreClamped) {
    nlohmann::json source = WallpaperRotationSettings{}.to_json();
    source["interval_minutes"] = 100000;
    source["pool_weights"]["local-live"] = 9000;
    source["pool_weights"]["online-static"] = -50;
    source["enabled"] = "yes";

    const auto restored = WallpaperRotationSettings::from_json(source);
    EXPECT_EQ(
        restored.interval_minutes, WallpaperRotationSettings::kMaximumIntervalMinutes
    );
    EXPECT_EQ(restored.pool_weights[1], 100);
    EXPECT_EQ(restored.pool_weights[2], 0);
    EXPECT_FALSE(restored.enabled);
}

// A newer schema version loads best-effort: this build's defaults fill the
// gaps.
TEST(WallpaperRotationSettingsTest, ANewerSchemaStillLoads) {
    nlohmann::json source = WallpaperRotationSettings{}.to_json();
    source["schema_version"] = 99;
    source["enabled"] = true;

    const auto restored = WallpaperRotationSettings::from_json(source);
    EXPECT_TRUE(restored.enabled);
    EXPECT_TRUE(restored.has_enabled_pool());
}

TEST(WallpaperRotationSettingsTest, HistoryIsBoundedWhenReading) {
    constexpr std::size_t kEntries = 200;
    WallpaperRotationSettings settings;
    for (std::size_t index = 0; index < kEntries; ++index) {
        settings.history.push_back("/wallpapers/" + std::to_string(index) + ".png");
    }
    const auto restored =
        WallpaperRotationSettings::from_json(settings.to_json());
    EXPECT_LE(restored.history.size(), 4 * WallpaperRotationSettings::kDefaultHistoryLimit);
}

TEST(WallpaperRotationSettingsTest, DefaultRootsComeFromTheEnvironment) {
    ScopedState state;
    EXPECT_EQ(default_static_wallpaper_root(), state.root() / "static");
    EXPECT_EQ(default_live_wallpaper_root(), state.root() / "live");
}

// The state path override is what makes every other test in this file safe.
TEST(WallpaperRotationSettingsTest, StatePathFollowsTheOverride) {
    ScopedState state;
    EXPECT_EQ(
        WallpaperRotationStore::default_path(), state.root() / "rotation.json"
    );
}
