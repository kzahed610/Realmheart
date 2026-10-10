#include "wallpaper-native/source/WallpaperMediaProbe.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

using realmheart::wallpaper_native::probe_wallpaper_media;
using realmheart::wallpaper_native::probe_wallpaper_media_bytes;
using realmheart::wallpaper_native::WallpaperMediaProbe;
using realmheart::wallpaper_native::WallpaperMediaType;

namespace {

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        std::string pattern = "/tmp/realmheart-media-probe-XXXXXX";
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
    }

    ~ScopedTempDirectory() { std::filesystem::remove_all(directory_); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return directory_;
    }

    void write_text(const std::string& name, const std::string& contents) {
        std::ofstream output(directory_ / name, std::ios::binary);
        output << contents;
    }

private:
    std::filesystem::path directory_;
};

[[maybe_unused]] bool command_available(const char* name) {
    const std::string probe = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

// A tiny deterministic video, built at runtime because a committed binary
// fixture is harder to review than the two-line command that makes it.
// The container is forced so the fixture can also be written under a
// deliberately wrong file extension.
[[maybe_unused]] bool generate_test_video(const std::filesystem::path& destination) {
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -y"
        " -f lavfi -i \"testsrc=size=64x64:rate=10:duration=0.5\""
        " -c:v libx264 -pix_fmt yuv420p -frames:v 5 -f mp4 " +
        destination.string() + " 2>/dev/null";
    return std::system(command.c_str()) == 0;
}

} // namespace

// Classification is content-based: an image is an image whatever it is called.
TEST(WallpaperMediaProbeTest, ClassifiesARealImageAsStatic) {
    const auto probe = probe_wallpaper_media(REALMHEART_TEST_PROBE_IMAGE);
    EXPECT_EQ(probe.type, WallpaperMediaType::StaticImage);
    EXPECT_TRUE(probe.reason.empty());
}

TEST(WallpaperMediaProbeTest, RejectsAnEmptyPath) {
    const auto probe = probe_wallpaper_media("");
    EXPECT_EQ(probe.type, WallpaperMediaType::Unsupported);
    EXPECT_FALSE(probe.reason.empty());
}

TEST(WallpaperMediaProbeTest, RejectsAMissingFile) {
    const auto probe = probe_wallpaper_media("/nonexistent/realmheart/wallpaper.png");
    EXPECT_EQ(probe.type, WallpaperMediaType::Unsupported);
    EXPECT_EQ(probe.reason, "wallpaper file does not exist");
}

TEST(WallpaperMediaProbeTest, RejectsADirectory) {
    ScopedTempDirectory directory;
    const auto probe = probe_wallpaper_media(directory.path().string());
    EXPECT_EQ(probe.type, WallpaperMediaType::Unsupported);
    EXPECT_EQ(probe.reason, "wallpaper path is not a regular file");
}

TEST(WallpaperMediaProbeTest, RejectsATextFileWithAReason) {
    ScopedTempDirectory directory;
    directory.write_text("notes.txt", "not a wallpaper\n");
    const auto probe = probe_wallpaper_media((directory.path() / "notes.txt").string());
    EXPECT_EQ(probe.type, WallpaperMediaType::Unsupported);
    EXPECT_FALSE(probe.reason.empty());
}

// The live-only classification path. Without live support compiled in these
// expectations invert, which is exactly what the build option controls.
#if REALMHEART_HAVE_LIVE_WALLPAPER

TEST(WallpaperMediaProbeTest, ClassifiesAVideoFileAsVideo) {
    if (!command_available("ffmpeg")) {
        GTEST_SKIP() << "ffmpeg is unavailable; cannot build a video fixture";
    }
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(generate_test_video(video)) << "ffmpeg could not build the fixture";

    const auto probe = probe_wallpaper_media(video.string());
    ASSERT_EQ(probe.type, WallpaperMediaType::Video);
    // The detail line is what Doctor and the logs report about the source.
    EXPECT_NE(probe.detail.find("64x64"), std::string::npos);
    EXPECT_NE(probe.detail.find("fps"), std::string::npos);
}

TEST(WallpaperMediaProbeTest, VideoClassificationFollowsContentNotExtension) {
    if (!command_available("ffmpeg")) {
        GTEST_SKIP() << "ffmpeg is unavailable; cannot build a video fixture";
    }
    ScopedTempDirectory directory;
    // Deliberately named like an image: the probe must not trust the suffix.
    const auto mislabelled = directory.path() / "wallpaper.png";
    ASSERT_TRUE(generate_test_video(mislabelled)) << "ffmpeg could not build the fixture";

    EXPECT_EQ(
        probe_wallpaper_media(mislabelled.string()).type,
        WallpaperMediaType::Video
    );
}

TEST(WallpaperMediaProbeTest, OwnedVideoBytesAreReportedAsVideo) {
    if (!command_available("ffmpeg")) {
        GTEST_SKIP() << "ffmpeg is unavailable; cannot build a video fixture";
    }
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(generate_test_video(video)) << "ffmpeg could not build the fixture";

    std::ifstream input(video, std::ios::binary);
    const std::string bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
    ASSERT_FALSE(bytes.empty());

    // The contract is that owned video payloads are rejected by the caller with
    // a specific message; this probe's job is only to identify them.
    EXPECT_EQ(
        probe_wallpaper_media_bytes(bytes).type,
        WallpaperMediaType::Video
    );
}

#else

TEST(WallpaperMediaProbeTest, VideoIsUnsupportedWithoutLiveSupport) {
    const auto probe = probe_wallpaper_media("/nonexistent/realmheart/wallpaper.mp4");
    EXPECT_EQ(probe.type, WallpaperMediaType::Unsupported);
    EXPECT_EQ(probe.reason, "wallpaper file does not exist");
}

#endif

TEST(WallpaperMediaProbeTest, OwnedImageBytesAreNotAVideoPayload) {
    ScopedTempDirectory directory;
    directory.write_text("image.bin", "not a container at all");
    const auto probe = probe_wallpaper_media_bytes("not a container at all");
    EXPECT_EQ(probe.type, WallpaperMediaType::Unsupported);
}

TEST(WallpaperMediaProbeTest, OwnedEmptyBytesAreRejected) {
    const auto probe = probe_wallpaper_media_bytes({});
    EXPECT_EQ(probe.type, WallpaperMediaType::Unsupported);
    EXPECT_FALSE(probe.reason.empty());
}
