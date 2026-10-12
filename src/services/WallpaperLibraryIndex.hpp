#pragma once

#include "services/WallpaperDiscovery.hpp"
#include "services/WallpaperRotationSettings.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace realmheart::services {

// What a wallpaper is, as far as rotation is concerned. Unknown means "live
// candidate": the renderer decides at apply time and reports back, and the
// index corrects itself then (architecture document section 5.2).
enum class WallpaperMediaKind {
    Static,
    Live,
    Unknown,
};

// Where a wallpaper came from. Online wallpapers are cached local wallpapers:
// once a provider has synced a file into the cache, rotation treats it exactly
// like a file in a directory, so this is a tag on a local file and rotation
// never makes a network call (architecture document section 5).
enum class WallpaperOrigin {
    Local,
    CachedOnline,
};

struct WallpaperLibraryEntry {
    std::filesystem::path path;
    WallpaperMediaKind kind = WallpaperMediaKind::Unknown;
    WallpaperOrigin origin = WallpaperOrigin::Local;
    std::uint64_t file_size = 0;
    std::int64_t mtime = 0;
    // size + mtime identity, so a file replaced at the same path is a different
    // wallpaper and a stale classification cannot follow it.
    std::uint64_t fingerprint = 0;
};

struct WallpaperLibraryRoots {
    std::filesystem::path static_root;
    std::filesystem::path live_root;
    std::filesystem::path cache_root;

    // A root that is empty means "use the default", which is why every accessor
    // resolves rather than storing.
    [[nodiscard]] std::filesystem::path effective_static_root() const;
    [[nodiscard]] std::filesystem::path effective_live_root() const;
};

struct WallpaperLibrarySnapshot {
    std::vector<WallpaperLibraryEntry> entries;
    std::vector<std::string> diagnostics;
    // How long the sweep took, so the settings surface can say so.
    std::uint64_t scan_millis = 0;
};

// The unified wallpaper library: what wallpapers exist, what they are, and
// where they came from.
//
// A cheap index, not a decoder: classification reads at most a header, so a
// library with thousands of entries stays cheap to rebuild. Full rescans are
// bounded by an entry cap.
class WallpaperLibraryIndex {
public:
    // Default cap on entries per sweep. Generous on purpose: a hard failure on
    // a big collection is worse than a slower sweep.
    static constexpr std::size_t kDefaultEntryCap = 20'000;

    WallpaperLibraryIndex() = default;
    explicit WallpaperLibraryIndex(WallpaperLibraryRoots roots);

    // Sweeps every root and rebuilds the index. Roots that cannot be read are
    // reported as diagnostics; the sweep still succeeds.
    [[nodiscard]] WallpaperLibrarySnapshot rescan();

    // The renderer's answer for a source it actually played. Corrects the index
    // so a misclassified file moves to the right pool instead of being skipped
    // forever.
    void record_renderer_classification(
        const std::filesystem::path& path,
        WallpaperMediaKind kind
    );

    // pool() is a filter over the index, so it never touches the filesystem.
    [[nodiscard]] std::vector<WallpaperLibraryEntry> pool(
        WallpaperPool pool
    ) const;

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::vector<WallpaperLibraryEntry>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] const WallpaperLibraryRoots& roots() const noexcept {
        return roots_;
    }

    // The path of an entry, if it is still in the index. Used by rotation to
    // check that what it picked is still there.
    [[nodiscard]] const WallpaperLibraryEntry* find(
        const std::filesystem::path& path
    ) const;

private:
    // Classification of one discovered file. Cheap by construction.
    [[nodiscard]] WallpaperMediaKind classify(
        const std::filesystem::path& path
    ) const;

    WallpaperLibraryRoots roots_;
    std::vector<WallpaperLibraryEntry> entries_;
    std::vector<std::string> diagnostics_;
    std::uint64_t last_scan_millis_ = 0;
};

// The managed download cache (/local/share/realmheart/wallpaper-cache by
// default), where providers sync online wallpapers.
[[nodiscard]] std::filesystem::path default_wallpaper_cache_root();

} // namespace realmheart::services
