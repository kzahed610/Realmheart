#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>

namespace realmheart::services {

// Long-edge pixel size of the poster frame used as a palette source. It only
// has to average a video's colors, so it stays far below output resolution.
[[nodiscard]] constexpr int wallpaper_poster_palette_dimension() noexcept {
    return 512;
}

// Cache bounds. A poster is one small PNG per (video, dimension); the point of
// the cache is that theming a live wallpaper never re-decodes the video.
[[nodiscard]] constexpr std::size_t wallpaper_poster_cache_max_entries() noexcept {
    return 256;
}

// The one owner of the "this wallpaper is a live video" list.
//
// The selector previews live wallpapers from a poster frame, the theme path uses
// that same frame as the matugen input, and the selector's cover sizing asks the
// same question. Two copies of this list is how a video ends up discovered but
// neither previewable nor themable. Extensions are hints; the renderer's media
// probe remains the authority for what actually plays.
[[nodiscard]] bool is_live_wallpaper_source(
    const std::filesystem::path& path
) noexcept;

// The representative still frame of a live wallpaper, as a PNG on disk.
//
// Posters are cached under the user cache directory and keyed by the source's
// canonical path plus its size and mtime, so a video replaced at the same path
// never reuses a stale frame. Requesting an already-cached poster does not run
// ffmpeg.
class WallpaperPosterCache {
public:
    // Where a poster for 'video' at 'target_dimension' would live. The file only
    // exists after a successful extraction.
    [[nodiscard]] static std::filesystem::path poster_path_for(
        const std::filesystem::path& video,
        int target_dimension
    );

    // Extracts, or reuses, a poster frame. A 'target_dimension' of zero asks for
    // the palette-size poster. Returns an empty path with a human-readable
    // reason on failure; callers keep the previous palette instead of failing
    // the wallpaper.
    [[nodiscard]] static std::optional<std::filesystem::path> poster_frame_for(
        const std::filesystem::path& video,
        int target_dimension = 0,
        std::string* error_message = nullptr
    );

    // The palette input for any wallpaper: the wallpaper itself when it is a
    // still image, and its poster frame when it is a live video. BUG-3 was that
    // matugen panics on a video file's extension, so the video was passed
    // through, palette generation failed, and the desktop never re-themed to a
    // live wallpaper.
    [[nodiscard]] static std::optional<std::filesystem::path> palette_source_for(
        const std::filesystem::path& wallpaper,
        std::string* error_message = nullptr
    );

    // Drops the oldest posters past the cache bound. Never touches a poster
    // newer than the newest survivors.
    static void prune();

private:
    [[nodiscard]] static std::uint64_t source_fingerprint(
        const std::filesystem::path& video
    ) noexcept;
};

} // namespace realmheart::services
