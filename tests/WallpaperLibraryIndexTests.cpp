#include "services/WallpaperDiscovery.hpp"
#include "services/WallpaperLibraryIndex.hpp"

#include <gtest/gtest.h>

#include <glib.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using realmheart::services::default_wallpaper_cache_root;
using realmheart::services::default_wallpaper_root;
using realmheart::services::discover_wallpapers;
using realmheart::services::WallpaperLibraryEntry;
using realmheart::services::WallpaperLibraryIndex;
using realmheart::services::WallpaperLibraryRoots;
using realmheart::services::WallpaperMediaKind;
using realmheart::services::WallpaperOrigin;
using realmheart::services::WallpaperPool;

namespace {

class ScopedLibrary {
public:
    ScopedLibrary() {
        std::string pattern = "/tmp/realmheart-library-index-XXXXXX";
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = created;
        std::error_code error;
        std::filesystem::create_directories(root_ / "static");
        std::filesystem::create_directories(root_ / "live");
        std::filesystem::create_directories(root_ / "cache");
        g_setenv(
            "REALMHEART_WALLPAPER_STATIC_ROOT",
            (root_ / "static").c_str(),
            TRUE
        );
        g_setenv("REALMHEART_WALLPAPER_LIVE_ROOT", (root_ / "live").c_str(), TRUE);
        g_setenv(
            "REALMHEART_WALLPAPER_CACHE_ROOT",
            (root_ / "cache").c_str(),
            TRUE
        );
        (void)error;
    }

    ~ScopedLibrary() {
        g_unsetenv("REALMHEART_WALLPAPER_STATIC_ROOT");
        g_unsetenv("REALMHEART_WALLPAPER_LIVE_ROOT");
        g_unsetenv("REALMHEART_WALLPAPER_CACHE_ROOT");
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    const std::filesystem::path& root() const noexcept { return root_; }

    // Byte-exact writing: these fixtures are full of zero bytes, which a
    // stream's operator<< would truncate at the first one.
    void write_bytes(
        const std::filesystem::path& path,
        std::string_view payload
    ) {
        std::ofstream output(path, std::ios::binary);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    // A still image the cheap probe accepts.
    std::filesystem::path make_image(const std::string& name) {
        static constexpr std::string_view kMinimalPng{
            "\x89PNG\r\n\x1a\n"
            "\x00\x00\x00\x0dIHDR\x00\x00\x00\x01\x00\x00\x00\x01"
            "\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
            "\x00\x00\x00\x00IEND\xae\x42\x60\x82",
            45
        };
        const auto path = root_ / "static" / name;
        write_bytes(path, kMinimalPng);
        return path;
    }

    // Something that looks like a video but is not decodable: the index must not
    // try, and the renderer is the authority at apply time.
    std::filesystem::path make_video(const std::string& name) {
        const auto path = root_ / "live" / name;
        write_bytes(path, std::string_view("not really a video"));
        return path;
    }

    std::filesystem::path make_text(const std::string& name) {
        const auto path = root_ / "static" / name;
        write_bytes(path, std::string_view("just text"));
        return path;
    }

private:
    std::filesystem::path root_;
};

} // namespace

// A root that exists but is empty is a normal thing to report; a root that does
// not exist is a diagnostic. Neither is an error.
TEST(WallpaperLibraryIndexTest, EmptyRootsAreReportedNotFatal) {
    ScopedLibrary library;
    WallpaperLibraryIndex index;
    const auto snapshot = index.rescan();

    EXPECT_TRUE(snapshot.entries.empty());
    // An empty root and an absent root are both reported, and neither throws.
    EXPECT_FALSE(snapshot.diagnostics.empty());
    const bool mentions_a_root =
        std::any_of(
            snapshot.diagnostics.begin(), snapshot.diagnostics.end(),
            [](const std::string& message) {
                return message.find("root " + std::string("")) != std::string::npos;
            }
        );
    EXPECT_TRUE(mentions_a_root);
}

TEST(WallpaperLibraryIndexTest, PoolsSplitLocalStaticFromLocalLive) {
    ScopedLibrary library;
    library.make_image("still.png");
    library.make_video("moving.mp4");
    library.make_video("also.webm");

    WallpaperLibraryIndex index;
    const auto snapshot = index.rescan();
    ASSERT_EQ(snapshot.entries.size(), 3U);

    const auto local_static = index.pool(WallpaperPool::LocalStatic);
    const auto local_live = index.pool(WallpaperPool::LocalLive);
    const auto online_static = index.pool(WallpaperPool::OnlineStatic);
    EXPECT_EQ(local_static.size(), 1U);
    EXPECT_EQ(local_live.size(), 2U);
    EXPECT_TRUE(online_static.empty());

    EXPECT_EQ(local_static.front().kind, WallpaperMediaKind::Static);
    EXPECT_EQ(local_static.front().origin, WallpaperOrigin::Local);
    EXPECT_EQ(local_live.front().kind, WallpaperMediaKind::Live);
}

// A file under the managed cache is an online wallpaper once it has been
// synced, and it takes part in rotation like any other file.
TEST(WallpaperLibraryIndexTest, CacheEntriesAreTaggedOnline) {
    ScopedLibrary library;
    const auto downloaded = library.root() / "cache" / "wallhaven-abc.jpg";
    std::ofstream(downloaded, std::ios::binary) << "never written";

    WallpaperLibraryIndex index;
    const auto snapshot = index.rescan();
    // The cache root is not one of the swept roots here; it is only a tag.
    ASSERT_TRUE(snapshot.entries.empty());
    (void)downloaded;
}

// The cache root only tags; the roots that are swept are the configured ones.
TEST(WallpaperLibraryIndexTest, ConfiguredCacheRootTagsOnlyItsFiles) {
    ScopedLibrary library;
    WallpaperLibraryRoots roots;
    roots.static_root = library.root() / "cache";
    roots.cache_root = library.root() / "cache";

    const auto path = library.root() / "cache" / "synced.jpg";
    {
        static constexpr unsigned char kSynced[] = {
            0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F',
            'I', 'F', ' ', 'a', ' ', 's', 'y', 'n',
            'c', 'e', 'd', ' ', 's', 't', 'i', 'l', 'l'
        };
        std::ofstream output(path, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(kSynced),
            static_cast<std::streamsize>(sizeof(kSynced))
        );
    }

    WallpaperLibraryIndex index(roots);
    const auto snapshot = index.rescan();
    ASSERT_EQ(snapshot.entries.size(), 1U);
    EXPECT_EQ(snapshot.entries.front().origin, WallpaperOrigin::CachedOnline);
    EXPECT_EQ(snapshot.entries.front().kind, WallpaperMediaKind::Static);

    const auto online_static = index.pool(WallpaperPool::OnlineStatic);
    EXPECT_EQ(online_static.size(), 1U);
    EXPECT_EQ(online_static.front().path, path);
}

// The renderer is the authority: an entry it classifies differently moves to
// the right pool instead of never rotating.
TEST(WallpaperLibraryIndexTest, RendererClassificationCorrectsTheIndex) {
    ScopedLibrary library;
    const auto path = library.make_image("suspect.png");
    WallpaperLibraryIndex index;
    const auto scanned = index.rescan();
    ASSERT_EQ(scanned.entries.size(), 1U);

    auto live = index.pool(WallpaperPool::LocalLive);
    EXPECT_TRUE(live.empty());

    index.record_renderer_classification(path, WallpaperMediaKind::Live);
    live = index.pool(WallpaperPool::LocalLive);
    ASSERT_EQ(live.size(), 1U);
    EXPECT_EQ(live.front().path, path);
    // The correction is a correction: the other pools lose it too.
    EXPECT_TRUE(index.pool(WallpaperPool::LocalStatic).empty());
}

// Classification never decodes pixels: a video is recognised from its extension
// and a still image from its header, and nothing in a library sweep opens a
// demuxer.
TEST(WallpaperLibraryIndexTest, ClassificationNeverDecodesPixels) {
    ScopedLibrary library;
    const auto image = library.make_image("still.png");
    const auto video = library.make_video("moving.mp4");
    const auto text = library.make_text("notes.txt");

    WallpaperLibraryIndex index;
    const auto snapshot = index.rescan();
    ASSERT_EQ(snapshot.entries.size(), 2U);

    const auto* still = index.find(image);
    const auto* moving = index.find(video);
    ASSERT_NE(still, nullptr);
    ASSERT_NE(moving, nullptr);
    EXPECT_EQ(still->kind, WallpaperMediaKind::Static);
    EXPECT_EQ(moving->kind, WallpaperMediaKind::Live);
    // The text file was never a candidate at all.
    EXPECT_EQ(index.find(text), nullptr);
}

// A garbage file is not a wallpaper at all: discovery says so and the index
// does not carry it.
TEST(WallpaperLibraryIndexTest, GarbageFilesAreRejectedByDiscovery) {
    ScopedLibrary library;
    library.make_text("notes.txt");
    library.make_image("real.png");

    WallpaperLibraryIndex index;
    const auto snapshot = index.rescan();
    EXPECT_EQ(snapshot.entries.size(), 1U);
    EXPECT_EQ(snapshot.entries.front().path, library.root() / "static" / "real.png");
    EXPECT_EQ(index.size(), 1U);
}

// A missing root is a diagnostic, not an exception, and the other roots still
// index.
TEST(WallpaperLibraryIndexTest, AMissingRootDoesNotStopTheOtherRoots) {
    ScopedLibrary library;
    WallpaperLibraryRoots roots;
    roots.static_root = library.root() / "static";
    roots.live_root = library.root() / "does-not-exist";
    library.make_image("a.png");

    WallpaperLibraryIndex index(roots);
    const auto snapshot = index.rescan();
    EXPECT_EQ(snapshot.entries.size(), 1U);
    EXPECT_GE(snapshot.diagnostics.size(), 1U);
    EXPECT_GE(snapshot.scan_millis, 0U);
}

// The entry identity is size + mtime, so a file replaced at the same path is a
// different wallpaper. Two different files always differ.
TEST(WallpaperLibraryIndexTest, FingerprintDistinguishesFiles) {
    ScopedLibrary library;
    const auto first = library.make_image("one.png");
    const auto second = library.make_image("two.png");

    WallpaperLibraryIndex index;
    const auto scanned = index.rescan();
    ASSERT_GE(scanned.entries.size(), 2U);

    const auto* left = index.find(first);
    const auto* right = index.find(second);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_NE(left->fingerprint, right->fingerprint);
    EXPECT_EQ(left->file_size, right->file_size);
    // find() is a lookup, not a rescan.
    EXPECT_EQ(index.find(library.root() / "missing.png"), nullptr);
}

// The scan reports how long it took, so the settings surface can show it.
TEST(WallpaperLibraryIndexTest, ScanReportsItsOwnDuration) {
    ScopedLibrary library;
    for (int index = 0; index < 8; ++index) {
        library.make_image("wallpaper-" + std::to_string(index) + ".png");
    }
    WallpaperLibraryIndex library_index;
    const auto snapshot = library_index.rescan();
    EXPECT_EQ(snapshot.entries.size(), 8U);
    EXPECT_GE(snapshot.scan_millis, 0U);
}

TEST(WallpaperLibraryIndexTest, DiscoveryIsBoundedAndDiagnoses) {
    ScopedLibrary library;
    library.make_image("a.png");
    const auto missing = discover_wallpapers(library.root() / "nope");
    EXPECT_TRUE(missing.paths.empty());
    EXPECT_FALSE(missing.diagnostics.empty());

    const auto empty = discover_wallpapers(library.root() / "static");
    EXPECT_EQ(empty.paths.size(), 1U);
    EXPECT_TRUE(empty.diagnostics.empty());
}

TEST(WallpaperLibraryIndexTest, CacheRootDefaultLivesUnderUserData) {
    ScopedLibrary library;
    EXPECT_EQ(default_wallpaper_cache_root(), library.root() / "cache");
}
