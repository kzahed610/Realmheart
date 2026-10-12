#include "services/WallpaperRotator.hpp"

#include "services/WallpaperRotationSettings.hpp"

#include <gtest/gtest.h>

#include <glib.h>

#include <algorithm>
#include <fstream>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using realmheart::services::RotationChoice;
using realmheart::services::RotationPauseReason;
using realmheart::services::rotation_pause_reason_name;
using realmheart::services::WallpaperPool;
using realmheart::services::WallpaperLibraryIndex;
using realmheart::services::WallpaperRotationSettings;
using realmheart::services::WallpaperRotator;
using realmheart::services::WallpaperRotationStore;
using realmheart::services::choose_next_wallpaper;

namespace {

using Pool = WallpaperPool;
constexpr auto kPoolCount = realmheart::services::wallpaper_pool_count();

// A pool set built from paths, in pool order.
std::array<std::vector<std::filesystem::path>, kPoolCount> pools_of(
    const std::vector<std::string>& local_static,
    const std::vector<std::string>& local_live = {},
    const std::vector<std::string>& online_static = {},
    const std::vector<std::string>& online_live = {}
) {
    const auto make = [](const std::vector<std::string>& names) {
        std::vector<std::filesystem::path> paths;
        for (const auto& name : names) paths.emplace_back(name);
        return paths;
    };
    return {make(local_static), make(local_live), make(online_static),
            make(online_live)};
}

// A deterministic draw: one call returns a fixed value, everything after that
// the next queued value. Distribution tests queue a whole sequence.
class ScriptedRandom {
public:
    explicit ScriptedRandom(std::vector<double> values)
        : values_(std::move(values)) {}

    double operator()() const {
        // A draw past the end repeats the last value: a test that runs out of
        // script gets a stable answer rather than an undefined one.
        if (index_ < values_.size()) return values_[index_++];
        return values_.empty() ? 0.0 : values_.back();
    }

    [[nodiscard]] std::size_t calls() const noexcept { return index_; }

private:
    std::vector<double> values_;
    mutable std::size_t index_ = 0;
};

WallpaperRotationSettings enabled_settings() {
    WallpaperRotationSettings settings;
    settings.enabled = true;
    return settings;
}

} // namespace

TEST(RotationSelectionTest, DisabledRotationPicksNothing) {
    const auto pools = pools_of({"a.png"});
    const auto choice = choose_next_wallpaper(
        WallpaperRotationSettings{}, pools, {}, {}, [] { return 0.5; }
    );
    EXPECT_FALSE(choice.picked);
    EXPECT_FALSE(choice.reason.empty());
}

// With nothing to draw from, the reason says so and nothing is picked.
TEST(RotationSelectionTest, EmptyPoolsPickNothingWithAReason) {
    const auto choice = choose_next_wallpaper(
        enabled_settings(), pools_of({}), {}, {}, [] { return 0.5; }
    );
    EXPECT_FALSE(choice.picked);
    EXPECT_FALSE(choice.reason.empty());
}

TEST(RotationSelectionTest, AZeroWeightPoolIsNeverPicked) {
    WallpaperRotationSettings settings = enabled_settings();
    settings.pool_weights = {0, 100, 0, 0};
    const auto pools = pools_of({"a.png"}, {"b.mp4"});

    // Every draw value lands somewhere; the only pool with weight must win.
    for (const double draw : {0.0, 0.25, 0.5, 0.99}) {
        const auto choice = choose_next_wallpaper(
            settings, pools, {}, {}, [&draw] { return draw; }
        );
        ASSERT_TRUE(choice.picked) << draw;
        EXPECT_EQ(choice.pool, Pool::LocalLive);
        EXPECT_EQ(choice.path, "b.mp4");
    }
}

// Weights are a distribution over pools, not over files: with equal weights, a
// library with one static and three live wallpapers still gives the live pool
// half the draws. Each call consumes two draws (pool, then entry), so the
// scripted sequence is (pool draw, entry draw) repeated.
TEST(RotationSelectionTest, WeightsArePerPoolNotPerFile) {
    WallpaperRotationSettings settings = enabled_settings();
    settings.pool_weights = {50, 50, 0, 0};
    const auto pools = pools_of(
        {"/static/a.png"},
        {"/live/a.mp4", "/live/b.mp4", "/live/c.mp4"}
    );

    const ScriptedRandom random(
        std::vector<double>{
            0.0, 0.0, 0.9, 0.0, 0.0, 0.0, 0.9, 0.0
        }
    );
    int static_picks = 0;
    int live_picks = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto choice = choose_next_wallpaper(
            settings, pools, {}, {},
            [&random] { return random(); }
        );
        if (!choice.picked) continue;
        if (choice.pool == Pool::LocalStatic) ++static_picks;
        else ++live_picks;
    }
    // Half the draws each, which is what the weights say: a per-file split
    // would have given the live pool three quarters.
    EXPECT_EQ(static_picks, 2);
    EXPECT_EQ(live_picks, 2);
}

// No two outputs show the same wallpaper: a displayed entry is excluded from
// every other selection.
TEST(RotationSelectionTest, DisplayedEntriesAreExcludedEverywhere) {
    const auto pools = pools_of({"/a.png", "/b.png", "/c.png"});
    const std::vector<std::filesystem::path> displayed{"/b.png", "/c.png"};
    for (const double draw : {0.0, 0.5, 0.99}) {
        const auto choice = choose_next_wallpaper(
            enabled_settings(), pools, displayed, {},
            [&draw] { return draw; }
        );
        ASSERT_TRUE(choice.picked) << draw;
        EXPECT_EQ(choice.path, "/a.png");
    }
}

// A single-wallpaper library still rotates: the retry without the history
// exclusion exists for exactly this.
TEST(RotationSelectionTest, ASingleWallpaperStillRotates) {
    const auto pools = pools_of({"/only.png"});
    const WallpaperRotationSettings settings = enabled_settings();
    const std::vector<std::filesystem::path> history{"/only.png"};

    const auto choice = choose_next_wallpaper(
        settings, pools, {}, history, [] { return 0.5; }
    );
    EXPECT_TRUE(choice.picked);
    EXPECT_EQ(choice.path, "/only.png");
}

// A wallpaper inside the history window is retried without the history
// exclusion: a library with one wallpaper still has to be able to rotate.
TEST(RotationSelectionTest, HistoryWindowRetriesBeforeFallingThrough) {
    WallpaperRotationSettings settings = enabled_settings();
    settings.pool_weights = {50, 50, 0, 0};
    const auto pools = pools_of({"/static/a.png"}, {"/live/a.mp4"});
    const std::vector<std::filesystem::path> history{"/static/a.png"};

    const auto choice = choose_next_wallpaper(
        settings, pools, {}, history, [] { return 0.0; }
    );
    ASSERT_TRUE(choice.picked);
    EXPECT_EQ(choice.pool, Pool::LocalStatic);
    EXPECT_EQ(choice.path, "/static/a.png");
}

// A pool whose every entry is on screen has nothing to give: the draw falls
// through to the next pool by weight instead of failing the tick.
TEST(RotationSelectionTest, ADisplayedPoolFallsThroughToTheNextPool) {
    WallpaperRotationSettings settings = enabled_settings();
    settings.pool_weights = {50, 50, 0, 0};
    const auto pools = pools_of({"/static/a.png"}, {"/live/a.mp4"});
    const std::vector<std::filesystem::path> displayed{"/static/a.png"};

    const auto choice = choose_next_wallpaper(
        settings, pools, displayed, {}, [] { return 0.0; }
    );
    ASSERT_TRUE(choice.picked);
    EXPECT_EQ(choice.pool, Pool::LocalLive);
    EXPECT_EQ(choice.path, "/live/a.mp4");
}

// Two outputs are different names, and neither can pick what the other shows.
TEST(RotationSelectionTest, APoolCoveredByDisplaysPicksNothing) {
    const auto pools = pools_of({"/a.png", "/b.png"});
    const std::vector<std::filesystem::path> displayed{"/a.png", "/b.png"};

    const auto choice = choose_next_wallpaper(
        enabled_settings(), pools, displayed, {}, [] { return 0.5; }
    );
    EXPECT_FALSE(choice.picked);
    EXPECT_FALSE(choice.reason.empty());
}

// The choice carries the pool it came from, so the settings surface can say
// what rotation did.
TEST(RotationSelectionTest, ChoiceReportsThePoolItCameFrom) {
    const auto pools = pools_of({}, {"/live/a.mp4"});
    const auto choice = choose_next_wallpaper(
        enabled_settings(), pools, {}, {}, [] { return 0.5; }
    );
    ASSERT_TRUE(choice.picked);
    EXPECT_EQ(choice.pool, Pool::LocalLive);
    EXPECT_EQ(choice.path, "/live/a.mp4");
}

// An online-only library rotates: a cached wallpaper is a local wallpaper with
// an origin tag, and the pool filter is all that separates them.
TEST(RotationSelectionTest, OnlinePoolsRotateOnTheirOwn) {
    WallpaperRotationSettings settings = enabled_settings();
    settings.pool_weights = {0, 0, 0, 100};
    const auto pools = pools_of({}, {}, {}, {"/cache/a.mp4"});

    const auto choice = choose_next_wallpaper(
        settings, pools, {}, {}, [] { return 0.5; }
    );
    ASSERT_TRUE(choice.picked);
    EXPECT_EQ(choice.pool, Pool::OnlineLive);
    EXPECT_EQ(choice.path, "/cache/a.mp4");
}

// An empty pool's weight is not what picks it: only pools with entries can be
// drawn, so a library with no live wallpapers never produces a live pick.
TEST(RotationSelectionTest, AnEmptyPoolNeverWins) {
    WallpaperRotationSettings settings = enabled_settings();
    settings.pool_weights = {10, 90, 0, 0};
    const auto pools = pools_of({"/a.png"}, {});

    for (const double draw : {0.0, 0.5, 0.99}) {
        const auto choice = choose_next_wallpaper(
            settings, pools, {}, {}, [&draw] { return draw; }
        );
        ASSERT_TRUE(choice.picked) << draw;
        EXPECT_EQ(choice.pool, Pool::LocalStatic);
    }
}


// The full path, against a real store and a real index in a sandbox: a tick
// commits through the installed seam, the seam records what was applied, and
// the history grows. This is the "rotation is a caller, not a committer" test.
TEST(RotationRotatorTest, ATickCommitsThroughTheInstalledSeam) {
    std::string pattern = "/tmp/realmheart-rotator-XXXXXX";
    const char* created = ::mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    const auto root = std::filesystem::path(created);
    const auto rotate = std::shared_ptr<void>(nullptr, [root](void*) {
        g_unsetenv("REALMHEART_WALLPAPER_ROTATION_STATE");
        g_unsetenv("REALMHEART_WALLPAPER_STATIC_ROOT");
        g_unsetenv("REALMHEART_WALLPAPER_LIVE_ROOT");
        g_unsetenv("REALMHEART_WALLPAPER_CACHE_ROOT");
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    });
    (void)rotate;

    std::error_code error;
    std::filesystem::create_directories(root / "static", error);
    std::filesystem::create_directories(root / "live", error);
    ASSERT_FALSE(error);

    // Two still images, so selection has something to pick between.
    const auto write_image = [&root](const std::string& name) {
        const auto path = root / "static" / name;
        std::ofstream output(path, std::ios::binary);
        output << std::string("\x89PNG\r\n\x1a\n", 8) << std::string(64, 'x');
        return path;
    };
    const auto first = write_image("first.png");
    const auto second = write_image("second.png");
    ASSERT_GT(std::filesystem::file_size(first, error), 0U);

    g_setenv(
        "REALMHEART_WALLPAPER_STATIC_ROOT",
        (root / "static").c_str(),
        TRUE
    );
    g_setenv("REALMHEART_WALLPAPER_LIVE_ROOT", (root / "live").c_str(), TRUE);
    g_setenv(
        "REALMHEART_WALLPAPER_CACHE_ROOT", (root / "cache").c_str(), TRUE
    );
    g_setenv(
        "REALMHEART_WALLPAPER_ROTATION_STATE",
        (root / "rotation.json").c_str(),
        TRUE
    );

    auto store = std::make_shared<WallpaperRotationStore>();
    WallpaperRotationSettings settings;
    settings.enabled = true;
    settings.interval_minutes = 30;
    std::string save_error;
    ASSERT_TRUE(store->save(settings, &save_error)) << save_error;

    realmheart::services::WallpaperLibraryIndex index;
    std::string refresh_error;
    WallpaperRotator rotator(index, store);
    ASSERT_TRUE(rotator.refresh(&refresh_error)) << refresh_error;
    ASSERT_TRUE(rotator.settings().enabled);
    std::string start_error;
    ASSERT_TRUE(rotator.start(&start_error)) << start_error;

    std::vector<std::filesystem::path> applied;
    int completions = 0;
    rotator.set_apply_seam(
        [&applied, &completions](
            std::string_view, const std::filesystem::path& path,
            std::function<void(bool, std::string)> completion
        ) {
            applied.push_back(path);
            ++completions;
            if (completion) completion(true, {});
        }
    );
    // The rotator only applies what it selected, so the driver is the tick.
    static_cast<void>(index.rescan());

    rotator.tick();
    ASSERT_EQ(applied.size(), 1U);
    EXPECT_TRUE(applied.front() == first || applied.front() == second);
    EXPECT_GE(completions, 1);
}

// Rotation off means no apply at all: the seam is never called.
TEST(RotationRotatorTest, ADisabledRotatorNeverApplies) {
    std::string pattern = "/tmp/realmheart-rotator-disabled-XXXXXX";
    const char* created = ::mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    const auto root = std::filesystem::path(created);
    const auto rotate = std::shared_ptr<void>(nullptr, [root](void*) {
        g_unsetenv("REALMHEART_WALLPAPER_ROTATION_STATE");
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    });
    (void)rotate;

    g_setenv(
        "REALMHEART_WALLPAPER_ROTATION_STATE",
        (root / "rotation.json").c_str(),
        TRUE
    );
    auto store = std::make_shared<WallpaperRotationStore>();
    realmheart::services::WallpaperLibraryIndex index;

    std::string error;
    WallpaperRotator rotator(index, store);
    ASSERT_TRUE(rotator.refresh(&error)) << error;
    EXPECT_FALSE(rotator.settings().enabled);

    bool applied = false;
    rotator.set_apply_seam(
        [&applied](
            std::string_view, const std::filesystem::path&,
            std::function<void(bool, std::string)>
        ) { applied = true; }
    );
    EXPECT_FALSE(rotator.rotate_now(&error));
    EXPECT_FALSE(applied);
}

// Every pause reason has a name a user can read.
TEST(RotationPauseTest, PauseReasonsAreNamed) {
    using realmheart::services::RotationPauseReason;
    for (const auto reason : {
             RotationPauseReason::Disabled,
             RotationPauseReason::NoPool,
             RotationPauseReason::SelectorOpen,
             RotationPauseReason::SessionLocked,
             RotationPauseReason::ExclusiveFullscreen,
             RotationPauseReason::CriticalBattery,
             RotationPauseReason::ApplyInFlight,
             RotationPauseReason::OutputPaused,
         }) {
        EXPECT_FALSE(rotation_pause_reason_name(reason).empty());
    }
    EXPECT_EQ(rotation_pause_reason_name(RotationPauseReason::Disabled),
              "rotation is disabled");
}
