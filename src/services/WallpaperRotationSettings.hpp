#pragma once

#include <nlohmann_json/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace realmheart::services {

// The four pools rotation draws from (architecture document section 5.3).
//
// Online wallpapers are cached local wallpapers: once a provider has synced a
// file into the cache, rotation treats it exactly like a file in a directory,
// so a pool is a filter over the library and nothing more. Rotation never makes
// a network call.
enum class WallpaperPool {
    LocalStatic = 0,
    LocalLive,
    OnlineStatic,
    OnlineLive,
    Count
};

constexpr std::size_t wallpaper_pool_count() {
    return static_cast<std::size_t>(WallpaperPool::Count);
}

[[nodiscard]] std::string_view wallpaper_pool_name(WallpaperPool pool) noexcept;

// Rotation settings: what rotates, when, and what stops it.
//
// Persisted as versioned JSON with atomic writes, mirroring the
// GameMode/WallpaperService state patterns. Everything has a default that
// leaves rotation off, so a fresh install behaves exactly as it does today.
struct WallpaperRotationSettings {
    using PoolWeights = std::array<int, wallpaper_pool_count()>;

    static constexpr int kCurrentSchemaVersion = 1;
    static constexpr int kDefaultIntervalMinutes = 30;
    static constexpr int kMinimumIntervalMinutes = 5;
    static constexpr int kMaximumIntervalMinutes = 24 * 60;
    // Rotation is off until the user turns it on: no behaviour change ships by
    // accident (engine implementation plan, Stage 3 gate).
    static constexpr bool kDefaultEnabled = false;
    static constexpr std::size_t kDefaultHistoryLimit = 8;

    int schema_version = kCurrentSchemaVersion;
    bool enabled = kDefaultEnabled;
    int interval_minutes = kDefaultIntervalMinutes;

    // Where wallpapers come from. Empty means "unset": the library index falls
    // back to a default root, and the settings widget shows the effective path.
    std::filesystem::path static_root;
    std::filesystem::path live_root;

    // Per-pool selection weight, 0 disables the pool. Equal by default, so
    // enabling static and live is a per-pool split, not a per-file one.
    PoolWeights pool_weights{20, 20, 20, 20};

    // Rotation is suspended, never reset, while any of these hold.
    bool pause_when_selector_open = true;
    bool pause_when_locked = true;
    bool pause_when_exclusive_fullscreen = true;
    bool pause_on_critical_battery = true;

    // Outputs the user removed from global rotation. Rotation policy stays
    // global; selection still happens per output on the shared tick.
    std::set<std::string> paused_outputs;

    // Recently rotated wallpapers, newest last. Bounded, and shared across
    // outputs so two monitors never show the same wallpaper at once and a
    // restart does not immediately repeat.
    std::vector<std::string> history;

    [[nodiscard]] int pool_weight(WallpaperPool pool) const {
        const auto index = static_cast<std::size_t>(pool);
        return index < pool_weights.size() ? pool_weights[index] : 0;
    }

    // True when at least one pool has weight, so rotation has something to draw
    // from. The failure matrix treats "all pools empty" as a silent skip.
    [[nodiscard]] bool has_enabled_pool() const noexcept;

    [[nodiscard]] nlohmann::json to_json() const;
    // Unknown fields are ignored and out-of-range values are clamped rather
    // than rejected: a settings file from a newer build must not brick the
    // shell.
    static WallpaperRotationSettings from_json(
        const nlohmann::json& source,
        std::string* diagnostic = nullptr
    );
};

// Where the settings live, and how they are loaded and saved.
class WallpaperRotationStore {
public:
    // Any state home but the default: a test (or a user with a small volume)
    // can redirect it. The override is NOT given to the constructor because
    // callers pass paths around freely; it is resolved once per process.
    [[nodiscard]] static std::filesystem::path default_path();

    WallpaperRotationStore(std::filesystem::path path = {});
    ~WallpaperRotationStore() = default;

    // Loads the settings, falling back to defaults with a diagnostic on a
    // missing, unreadable, or corrupt file. Never throws, never deletes user
    // data.
    [[nodiscard]] WallpaperRotationSettings load(std::string* diagnostic = nullptr) const;

    // Saves atomically (temp file + rename). Returns false with a reason when
    // the write fails; the previous settings survive.
    [[nodiscard]] bool save(
        const WallpaperRotationSettings& settings,
        std::string* error_message = nullptr
    ) const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// Default wallpaper roots, used when the setting is empty.
[[nodiscard]] std::filesystem::path default_static_wallpaper_root();
[[nodiscard]] std::filesystem::path default_live_wallpaper_root();

} // namespace realmheart::services
