#include "wallpaper-native/video/VideoDecoder.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

using realmheart::wallpaper_native::VideoDecoder;
using realmheart::wallpaper_native::VideoFrame;
using realmheart::wallpaper_native::VideoFrameFormat;

namespace {

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        std::string pattern = "/tmp/realmheart-video-decoder-XXXXXX";
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
    }

    ~ScopedTempDirectory() { std::filesystem::remove_all(directory_); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return directory_;
    }

private:
    std::filesystem::path directory_;
};

[[maybe_unused]] bool ffmpeg_available() {
    return std::system("command -v ffmpeg >/dev/null 2>&1") == 0;
}

// A tiny, deterministic, several-frame video. Built at runtime so the
// repository carries no opaque binary fixture.
[[maybe_unused]] bool build_fixture(
    const std::filesystem::path& destination,
    const std::string& filters
) {
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"" + filters +
        "\" -c:v libx264 -pix_fmt yuv420p -frames:v 5 -f mp4 " +
        destination.string() + " 2>/dev/null";
    return std::system(command.c_str()) == 0;
}

} // namespace

// Without live wallpaper support compiled in, the decoder must say so loudly
// rather than fail in some unspecified way.
#if REALMHEART_HAVE_LIVE_WALLPAPER

TEST(VideoDecoderTest, OpensAndDescribesAStream) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(build_fixture(video, "testsrc=size=64x64:rate=10:duration=0.5"));

    VideoDecoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(video.string(), &error)) << error;
    EXPECT_EQ(decoder.stream_info().width, 64);
    EXPECT_EQ(decoder.stream_info().height, 64);
    EXPECT_GT(decoder.stream_info().frame_rate, 0.0);
    EXPECT_FALSE(decoder.stream_info().codec_name.empty());
    EXPECT_TRUE(decoder.stream_info().has_timestamps);
    decoder.close();
}

TEST(VideoDecoderTest, DecodesSequentialFramesAndReportsEndOfStream) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(build_fixture(video, "testsrc=size=64x64:rate=10:duration=0.5"));

    VideoDecoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(video.string(), &error)) << error;

    std::vector<std::int64_t> presentation;
    int frames = 0;
    while (frames < 20) {
        VideoFrame frame;
        if (!decoder.decode_next(frame, &error)) break;
        if (!frame.valid()) continue;
        // NV12 is the working format: luma then interleaved chroma, 1.5 bytes
        // per pixel instead of 4, with the colour conversion left to the
        // shader.
        ASSERT_EQ(frame.format, VideoFrameFormat::Nv12);
        ASSERT_EQ(frame.width, 64);
        ASSERT_EQ(frame.height, 64);
        ASSERT_EQ(frame.pixels.size(), 64U * 64U + 64U * 64U / 2U);
        ASSERT_FALSE(frame.pixels.empty());
        presentation.push_back(frame.presentation_us);
        ++frames;
    }

    // The fixture is five frames; decoding must stop at the end of the stream
    // rather than looping on its own.
    EXPECT_EQ(frames, 5);
    EXPECT_TRUE(decoder.at_eof());

    // Presentation timestamps start at zero and increase monotonically.
    ASSERT_FALSE(presentation.empty());
    EXPECT_EQ(presentation.front(), 0);
    for (std::size_t index = 1; index < presentation.size(); ++index) {
        EXPECT_GE(presentation[index], presentation[index - 1]);
    }
    decoder.close();
}

TEST(VideoDecoderTest, LoopRestartsPlaybackFromZero) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(build_fixture(video, "testsrc=size=64x64:rate=10:duration=0.5"));

    VideoDecoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(video.string(), &error)) << error;

    const auto drain = [&decoder, &error]() {
        std::vector<std::int64_t> times;
        while (true) {
            VideoFrame frame;
            if (!decoder.decode_next(frame, &error)) break;
            if (frame.valid()) times.push_back(frame.presentation_us);
        }
        return times;
    };

    const auto first_pass = drain();
    ASSERT_EQ(first_pass.size(), 5U);
    EXPECT_EQ(first_pass.front(), 0);

    ASSERT_TRUE(decoder.restart_from_beginning(&error)) << error;
    EXPECT_FALSE(decoder.at_eof());

    const auto second_pass = drain();
    ASSERT_EQ(second_pass.size(), 5U);
    // A loop is a fresh timeline: the clock restarts at zero rather than
    // continuing from the end of the previous pass.
    EXPECT_EQ(second_pass.front(), 0);
    decoder.close();
}

TEST(VideoDecoderTest, RejectsAnAudioOnlyFile) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    ScopedTempDirectory directory;
    const auto audio = directory.path() / "audio-only.mp4";
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"sine=frequency=440:duration=0.5\""
        " -c:a aac -f mp4 " + audio.string() + " 2>/dev/null";
    ASSERT_EQ(std::system(command.c_str()), 0);

    VideoDecoder decoder;
    std::string error;
    EXPECT_FALSE(decoder.open(audio.string(), &error));
    EXPECT_FALSE(error.empty());
}

TEST(VideoDecoderTest, RejectsAMissingFile) {
    VideoDecoder decoder;
    std::string error;
    EXPECT_FALSE(decoder.open("/nonexistent/realmheart/video.mp4", &error));
    EXPECT_FALSE(error.empty());
}

TEST(VideoDecoderTest, CloseIsSafeToCallTwice) {
    VideoDecoder decoder;
    decoder.close();
    decoder.close();
    SUCCEED();
}

#else

TEST(VideoDecoderTest, ReportsThatDecodingIsNotCompiledIn) {
    VideoDecoder decoder;
    std::string error;
    EXPECT_FALSE(decoder.open("/any/path.mp4", &error));
    EXPECT_FALSE(error.empty());
}

#endif
