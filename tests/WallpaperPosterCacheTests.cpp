#include "services/WallpaperPosterCache.hpp"

#include <gtest/gtest.h>

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

using realmheart::services::is_live_wallpaper_source;
using realmheart::services::WallpaperPosterCache;
using realmheart::services::wallpaper_poster_cache_max_entries;
using realmheart::services::wallpaper_poster_palette_dimension;

namespace {

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        std::string pattern = "/tmp/realmheart-poster-cache-XXXXXX";
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = created;
    }

    ~ScopedTempDirectory() {
        g_unsetenv("REALMHEART_WALLPAPER_POSTER_CACHE");
        std::filesystem::remove_all(root_);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    // Redirects the cache into the sandbox so a test never touches the real
    // user cache directory.
    void isolate_poster_cache() {
        const auto cache = root_ / "posters";
        std::filesystem::create_directories(cache);
        g_setenv("REALMHEART_WALLPAPER_POSTER_CACHE", cache.c_str(), TRUE);
    }

private:
    std::filesystem::path root_;
};

[[maybe_unused]] bool ffmpeg_available() {
    return std::system("command -v ffmpeg >/dev/null 2>&1") == 0;
}

// A small, deterministic video, built at runtime: no binary fixture in the repo.
[[maybe_unused]] bool build_video(
    const std::filesystem::path& destination,
    const std::string& filters = "testsrc=size=64x64:rate=10:duration=0.5"
) {
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"" + filters +
        "\" -c:v libx264 -pix_fmt yuv420p -frames:v 5 -f mp4 " +
        destination.string() + " 2>/dev/null";
    return std::system(command.c_str()) == 0;
}

bool looks_like_png(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::file_size(path, error) < 8) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    char header[8]{};
    input.read(header, sizeof(header));
    return input.gcount() == static_cast<std::streamsize>(sizeof(header)) &&
           std::string_view(header, sizeof(header)) ==
               std::string_view("\x89PNG\r\n\x1a\n", 8);
}

// A poster only exists to be decoded. The theme path hands it to matugen, which
// requires an image, so "is it a real image" is the contract.
bool poster_decodes(const std::filesystem::path& path) {
    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &error);
    if (pixbuf == nullptr) {
        g_clear_error(&error);
        return false;
    }
    g_object_unref(pixbuf);
    g_clear_error(&error);
    return true;
}

// A trailer past the magic is what a truncated extraction looks like: the
// header is intact, the file is not.
bool poster_is_complete(const std::filesystem::path& path) {
    return poster_decodes(path);
}

} // namespace

// One owner of the live-wallpaper list: the selector's preview decision and the
// theme path's palette decision must never drift apart.
TEST(WallpaperPosterCacheTest, LiveWallpaperClassificationIsShared) {
    EXPECT_TRUE(is_live_wallpaper_source("/a/wallpaper.mp4"));
    EXPECT_TRUE(is_live_wallpaper_source("/a/wallpaper.MP4"));
    EXPECT_TRUE(is_live_wallpaper_source("/a/wallpaper.WebM"));
    EXPECT_TRUE(is_live_wallpaper_source("/a/wallpaper.mkv"));
    EXPECT_TRUE(is_live_wallpaper_source("/a/wallpaper.mov"));
    EXPECT_TRUE(is_live_wallpaper_source("/a/wallpaper.avi"));
    EXPECT_FALSE(is_live_wallpaper_source("/a/wallpaper.png"));
    EXPECT_FALSE(is_live_wallpaper_source("/a/wallpaper.JPG"));
    EXPECT_FALSE(is_live_wallpaper_source("/a/wallpaper.txt"));
    EXPECT_FALSE(is_live_wallpaper_source(""));
    EXPECT_FALSE(is_live_wallpaper_source("/a/wallpaper"));
}

TEST(WallpaperPosterCacheTest, PosterPathIsStablePerSourceAndDimension) {
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    std::ofstream(sandbox.root() / "wallpaper.mp4") << "video bytes";

    const auto video = sandbox.root() / "wallpaper.mp4";
    EXPECT_EQ(
        WallpaperPosterCache::poster_path_for(video, 512),
        WallpaperPosterCache::poster_path_for(video, 512)
    );
    // A different dimension is a different poster: the selector wants an
    // output-sized frame, theming wants a small one.
    EXPECT_NE(
        WallpaperPosterCache::poster_path_for(video, 512),
        WallpaperPosterCache::poster_path_for(video, 1024)
    );
}

// A video replaced at the same path is a different wallpaper, so it must not
// inherit the old poster's colors.
TEST(WallpaperPosterCacheTest, ReplacingTheVideoChangesItsPosterIdentity) {
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    const auto video = sandbox.root() / "wallpaper.mp4";
    {
        std::ofstream output(video, std::ios::binary);
        output << "first video bytes";
    }
    const auto first = WallpaperPosterCache::poster_path_for(video, 512);

    std::error_code error;
    std::filesystem::remove(video, error);
    // A new inode at the same path: the fingerprint has to notice.
    {
        std::ofstream output(video, std::ios::binary);
        output << "a much longer second video payload";
    }
    EXPECT_NE(first, WallpaperPosterCache::poster_path_for(video, 512));
}

TEST(WallpaperPosterCacheTest, MissingVideoReportsAReason) {
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    std::string error;
    const auto poster = WallpaperPosterCache::poster_frame_for(
        sandbox.root() / "absent.mp4", 0, &error
    );
    EXPECT_FALSE(poster.has_value());
    EXPECT_FALSE(error.empty());
}

TEST(WallpaperPosterCacheTest, EmptyPathReportsAReason) {
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    std::string error;
    EXPECT_FALSE(WallpaperPosterCache::poster_frame_for({}, 0, &error).has_value());
    EXPECT_FALSE(error.empty());
}

#if REALMHEART_HAVE_LIVE_WALLPAPER
// The palette source of a still image is the image itself: theming a static
// wallpaper must not change behaviour.
TEST(WallpaperPosterCacheTest, PaletteSourceOfAStillImageIsItself) {
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    const auto image = sandbox.root() / "wallpaper.png";
    std::ofstream(image, std::ios::binary) << "not really a png";
    EXPECT_EQ(WallpaperPosterCache::palette_source_for(image).value_or({}), image);
}
#endif

// BUG-3: matugen panics on a video file's extension, so the video path used to
// be handed to it and palette generation always failed. A live wallpaper must
// be themeded from a real image.
TEST(WallpaperPosterCacheTest, PaletteSourceOfALiveWallpaperIsAnImage) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    const auto video = sandbox.root() / "wallpaper.mp4";
    ASSERT_TRUE(build_video(video));

    std::string error;
    const auto source = WallpaperPosterCache::palette_source_for(video, &error);
    ASSERT_TRUE(source.has_value()) << error;
    EXPECT_NE(*source, video);
    EXPECT_EQ(source->extension(), ".png");
    ASSERT_TRUE(looks_like_png(*source));
    EXPECT_TRUE(poster_decodes(*source));
}

TEST(WallpaperPosterCacheTest, ExtractionProducesADecodablePoster) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    const auto video = sandbox.root() / "wallpaper.mp4";
    ASSERT_TRUE(build_video(video));

    std::string error;
    const auto poster = WallpaperPosterCache::poster_frame_for(video, 0, &error);
    ASSERT_TRUE(poster.has_value()) << error;
    EXPECT_EQ(*poster, WallpaperPosterCache::poster_path_for(video, 512));
    EXPECT_TRUE(looks_like_png(*poster));
    EXPECT_TRUE(poster_decodes(*poster));
    // No partial extraction is left behind.
    EXPECT_FALSE(std::filesystem::exists(poster->string() + ".partial"));
}

// The point of the cache: theming a live wallpaper twice must not decode the
// video twice. The mtime is the honest witness, and it needs no timing guess.
TEST(WallpaperPosterCacheTest, ASecondRequestReusesTheCachedPoster) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    const auto video = sandbox.root() / "wallpaper.mp4";
    ASSERT_TRUE(build_video(video));

    std::string error;
    const auto first = WallpaperPosterCache::poster_frame_for(video, 0, &error);
    ASSERT_TRUE(first.has_value()) << error;

    std::error_code filesystem_error;
    const auto modified = std::filesystem::last_write_time(*first, filesystem_error);
    ASSERT_FALSE(filesystem_error);

    const auto second = WallpaperPosterCache::poster_frame_for(video, 0, &error);
    ASSERT_TRUE(second.has_value()) << error;
    EXPECT_EQ(*second, *first);
    EXPECT_EQ(
        std::filesystem::last_write_time(*second, filesystem_error), modified
    );
}

// A truncated or half-written PNG in the cache is a failed extraction, not a
// poster: the next request has to replace it instead of theming from it.
TEST(WallpaperPosterCacheTest, ATruncatedPosterIsReplaced) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    const auto video = sandbox.root() / "wallpaper.mp4";
    ASSERT_TRUE(build_video(video));

    const auto poster = WallpaperPosterCache::poster_path_for(video, 512);
    std::filesystem::create_directories(poster.parent_path());
    {
        // A complete header, no chunks behind it: exactly what an interrupted
        // extraction leaves, and exactly what must not become a palette.
        std::ofstream truncated(poster, std::ios::binary);
        truncated << "\x89PNG\r\n\x1a\n";
    }
    ASSERT_TRUE(looks_like_png(poster));
    ASSERT_FALSE(poster_is_complete(poster));

    std::string error;
    const auto extracted = WallpaperPosterCache::poster_frame_for(video, 0, &error);
    ASSERT_TRUE(extracted.has_value()) << error;
    EXPECT_TRUE(poster_is_complete(*extracted));
}

TEST(WallpaperPosterCacheTest, PruneKeepsTheNewestPosters) {
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    const auto cache = sandbox.root() / "posters";
    std::filesystem::create_directories(cache);

    const auto total = wallpaper_poster_cache_max_entries() + 3U;
    std::vector<std::filesystem::path> posters;
    for (std::size_t index = 0; index < total; ++index) {
        const auto path = cache / ("poster-" + std::to_string(index) + ".png");
        std::ofstream(path, std::ios::binary) << "\x89PNG\r\n\x1a\npayload";
        std::error_code error;
        // Distinct mtimes, oldest first, so the expected survivors are known.
        const auto stamp = std::filesystem::file_time_type(
            std::chrono::file_clock::now() +
            std::chrono::hours(static_cast<int>(index))
        );
        std::filesystem::last_write_time(path, stamp, error);
        ASSERT_FALSE(error);
        posters.push_back(path);
    }

    WallpaperPosterCache::prune();

    for (std::size_t index = 0; index < 3U; ++index) {
        EXPECT_FALSE(std::filesystem::exists(posters[index]));
    }
    for (std::size_t index = 3U; index < total; ++index) {
        EXPECT_TRUE(std::filesystem::exists(posters[index]));
    }
}

TEST(WallpaperPosterCacheTest, PruneIgnoresAnEmptyCache) {
    ScopedTempDirectory sandbox;
    sandbox.isolate_poster_cache();
    WallpaperPosterCache::prune();
    SUCCEED();
}

// The palette-size poster is a documented constant; a change here changes what
// every live wallpaper's theme is derived from.
TEST(WallpaperPosterCacheTest, PalettePosterStaysSmall) {
    EXPECT_LE(wallpaper_poster_palette_dimension(), 1024);
    EXPECT_GT(wallpaper_poster_palette_dimension(), 0);
    EXPECT_GT(wallpaper_poster_cache_max_entries(), 8U);
}
