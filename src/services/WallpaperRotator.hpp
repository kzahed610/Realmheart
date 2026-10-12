#pragma once

#include "services/WallpaperLibraryIndex.hpp"
#include "services/WallpaperRotationSettings.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::services {

// What one selection decided, and what it was given to choose from.
struct RotationChoice {
    bool picked = false;
    // Why nothing was picked, when it was not. Empty on success.
    std::string reason;
    WallpaperPool pool = WallpaperPool::LocalStatic;
    std::filesystem::path path;
};

// Selection policy, kept free of the filesystem, the clock, and the controller
// so it can be tested exactly (architecture document section 6.2).
//
//   1. the enabled, non-empty pools, with empty pools' weight redistributed
//      proportionally;
//   2. a pool drawn by weight;
//   3. an entry drawn uniformly from that pool, excluding anything currently
//      displayed on any output and anything inside the shared history window;
//   4. a retry without the history exclusion, then a fall through to the next
//      pool by weight;
//   5. nothing at all, with a reason.
//
// 'random' must return a uniform value in [0,1); injecting it is what makes the
// distribution testable.
[[nodiscard]] RotationChoice choose_next_wallpaper(
    const WallpaperRotationSettings& settings,
    const std::array<std::vector<std::filesystem::path>, wallpaper_pool_count()>&
        pools,
    const std::vector<std::filesystem::path>& displayed,
    const std::vector<std::filesystem::path>& history,
    const std::function<double()>& random
);

// What rotation is waiting on. Every reason is observable, so the user can be
// told why the wallpaper is not changing instead of guessing.
enum class RotationPauseReason {
    Disabled,
    NoPool,
    SelectorOpen,
    SessionLocked,
    ExclusiveFullscreen,
    CriticalBattery,
    ApplyInFlight,
    OutputPaused,
};

[[nodiscard]] std::string_view rotation_pause_reason_name(
    RotationPauseReason reason
) noexcept;

// One rotation target. An empty output name means the global wallpaper.
struct RotationTarget {
    std::string output_name;
    std::optional<std::filesystem::path> current;
};

// The rotating side of the wallpaper engine.
//
// Owns the timer and the selection bookkeeping; it never commits a wallpaper
// itself. Every apply goes through WallpaperController, which stays the only
// component allowed to commit (architecture document section 18, invariant 1),
// and rotation always yields to a manual apply.
class WallpaperRotator {
public:
    using RandomSource = std::function<double()>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // 'index' is the library to choose from; it is owned elsewhere and must
    // outlive the rotator. 'apply' is the commit seam.
    WallpaperRotator(
        WallpaperLibraryIndex& index,
        std::shared_ptr<WallpaperRotationStore> settings_store
    );

    ~WallpaperRotator();

    WallpaperRotator(const WallpaperRotator&) = delete;
    WallpaperRotator& operator=(const WallpaperRotator&) = delete;

    // Reloads the settings and rebuilds the pools from the index. Returns false
    // with a reason when the settings are unusable, which leaves rotation
    // exactly as it was.
    [[nodiscard]] bool refresh(std::string* error_message = nullptr);

    // Arms the timer for the configured interval. Does nothing while paused.
    [[nodiscard]] bool start(std::string* error_message = nullptr);
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_; }

    // "Surprise me": rotate now, through the same selection path as a tick.
    [[nodiscard]] bool rotate_now(std::string* error_message = nullptr);

    // What would happen next, and why it is not happening.
    [[nodiscard]] RotationChoice peek() const;

    // Pause bookkeeping. A reason set to true is held until it is set to false;
    // several can be held at once.
    void set_paused(RotationPauseReason reason, bool paused = true);
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] std::vector<RotationPauseReason> pause_reasons() const;

    // The bookkeeping that keeps the selection honest: what each output is
    // showing. Called when a wallpaper is committed, by the engine and not by
    // rotation, so a manual apply moves the wallpaper out of rotation's way.
    void set_target_current(
        std::string_view output_name,
        const std::filesystem::path& path
    );
    void forget_target(std::string_view output_name);

    // Called after a rotation commit, so the history and the "pending" state
    // stay correct even when the commit failed.
    void note_apply_finished(
        std::string_view output_name,
        bool success,
        std::string_view error_message
    );

    // The settings as loaded, for the status surface.
    [[nodiscard]] WallpaperRotationSettings settings() const;
    // Seconds until the next tick, or nothing while paused or stopped.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
        next_tick() const;

    // Seams for tests: an injectable random source and clock.
    void set_random_source(RandomSource source);
    void set_clock(Clock clock);

    // The commit seam. Rotation never commits directly: the engine installs a
    // callback that goes through WallpaperController, which stays the only
    // component allowed to commit a wallpaper.
    using ApplySeam = std::function<void(
        std::string_view output_name,
        const std::filesystem::path& path,
        std::function<void(bool, std::string)> completion
    )>;
    void set_apply_seam(ApplySeam seam);

    // The timer callback the shell's main loop installs. Returns the source to
    // keep (G_SOURCE_CONTINUE / G_SOURCE_REMOVE) semantics through the caller.
    void tick();

private:
    [[nodiscard]] std::array<std::vector<std::filesystem::path>,
                             wallpaper_pool_count()>
        build_pools() const;

    // Calls the seam, or reports a clear failure when none is installed: a
    // rotator without a seam must never be mistaken for one that committed.
    void apply_choice(
        std::string_view output_name,
        const std::filesystem::path& path,
        std::function<void(bool, std::string)> completion
    );

    ApplySeam apply_seam_;

    WallpaperLibraryIndex& index_;
    std::shared_ptr<WallpaperRotationStore> settings_store_;
    WallpaperRotationSettings settings_;
    std::vector<RotationTarget> targets_;
    std::vector<RotationPauseReason> pauses_;
    std::vector<std::filesystem::path> history_;
    bool running_ = false;
    bool apply_in_flight_ = false;
    RandomSource random_;
    Clock clock_;
    std::chrono::steady_clock::time_point next_tick_{};
};

} // namespace realmheart::services
