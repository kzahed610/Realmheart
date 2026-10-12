#include "wallpaper-native/source/VideoWallpaperSource.hpp"
#include "wallpaper-native/video/VideoDecodeCoordinator.hpp"

#include <gtest/gtest.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using realmheart::wallpaper_native::VideoDecodeCoordinator;
using realmheart::wallpaper_native::VideoWallpaperSource;

namespace {

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        std::string pattern = "/tmp/realmheart-video-source-XXXXXX";
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

// A tiny, deterministic, several-frame video, built at runtime so the
// repository carries no opaque binary fixture.
[[maybe_unused]] bool build_fixture(
    const std::filesystem::path& destination,
    const std::string& filters
) {
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"" + filters +
        "\" -c:v libx264 -pix_fmt yuv420p -frames:v 8 -f mp4 " +
        destination.string() + " 2>/dev/null";
    return std::system(command.c_str()) == 0;
}

// Uploading a video frame needs a GL context. EGL provides one headlessly
// through Mesa's software renderer, which is enough for the upload rules.
class HeadlessGl {
public:
    HeadlessGl() {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY) return;
        EGLint major = 0;
        EGLint minor = 0;
        if (!eglInitialize(display_, &major, &minor)) {
            display_ = EGL_NO_DISPLAY;
            return;
        }
        eglBindAPI(EGL_OPENGL_ES_API);
        constexpr EGLint config_attributes[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLint config_count = 0;
        if (eglChooseConfig(
                display_, config_attributes, &config_, 1, &config_count
            ) != EGL_TRUE ||
            config_count == 0) {
            return;
        }
        constexpr EGLint surface_attributes[] = {
            EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE
        };
        surface_ = eglCreatePbufferSurface(display_, config_, surface_attributes);
        if (surface_ == EGL_NO_SURFACE) return;
        constexpr EGLint context_attributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
        };
        context_ = eglCreateContext(
            display_, config_, EGL_NO_CONTEXT, context_attributes
        );
        if (context_ == EGL_NO_CONTEXT) return;
        ready_ = eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
    }

    ~HeadlessGl() {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            eglTerminate(display_);
        }
    }

    HeadlessGl(const HeadlessGl&) = delete;
    HeadlessGl& operator=(const HeadlessGl&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    bool ready_ = false;
};

class BoundedWait {
public:
    explicit BoundedWait(std::chrono::milliseconds timeout)
        : deadline_(std::chrono::steady_clock::now() + timeout) {}

    // Polls 'predicate' until it holds or the budget runs out.
    void until(const std::function<bool()>& predicate) {
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    [[nodiscard]] bool expired() const noexcept {
        return std::chrono::steady_clock::now() >= deadline_;
    }

private:
    std::chrono::steady_clock::time_point deadline_;
};

// Presents whatever frame is due, which is what the renderer loop does on a
// presentation deadline.
void pump(VideoWallpaperSource& source) {
    for (int attempt = 0; attempt < 250; ++attempt) {
        (void)source.tick(std::chrono::steady_clock::now());
        if (source.current_frame().valid()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Drives presentation the way the renderer loop does, for a bounded wall time.
// Decoding is backpressured by the three-frame queue, so a source only decodes
// as far ahead as it is allowed to present: "decoding stops" is therefore
// something this has to check, not something it can assume away.
void drive(VideoWallpaperSource& source, std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
        (void)source.tick(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// A regression of BUG-1 used to hang the process rather than fail it: the
// decode worker kept using FFmpeg state that the re-open had freed. A hang is a
// useless signal in a test run, so this turns one into a loud failure.
class HangGuard {
public:
    explicit HangGuard(std::chrono::seconds timeout)
        : done_(std::make_shared<std::promise<void>>()) {
        const auto future = done_->get_future().share();
        watchdog_ = std::jthread([timeout, future](std::stop_token) {
            if (future.wait_for(timeout) == std::future_status::timeout) {
                std::fputs(
                    "VideoWallpaperSourceTest hung: the decode worker is stuck "
                    "on FFmpeg state that a re-open freed (BUG-1 regression)\n",
                    stderr
                );
                std::_Exit(3);
            }
        });
    }

    ~HangGuard() {
        if (done_) done_->set_value();
        watchdog_.request_stop();
        watchdog_.join();
    }

    HangGuard(const HangGuard&) = delete;
    HangGuard& operator=(const HangGuard&) = delete;

private:
    std::shared_ptr<std::promise<void>> done_;
    std::jthread watchdog_;
};

} // namespace

// Without live wallpaper support compiled in there is no decoder, so a source
// must fail loudly rather than pretend it can play.
#if REALMHEART_HAVE_LIVE_WALLPAPER

// A playing video source, ready to be tested. Decoding is backpressured by the
// three-frame queue, so a source only decodes as far ahead as it is allowed to
// present: every assertion here drives presentation the way the renderer loop
// does rather than assuming the worker runs free.
class PlayingSource {
public:
    explicit PlayingSource(const std::filesystem::path& video) {
        if (!ffmpeg_available()) {
            skip_reason_ = "ffmpeg unavailable";
            return;
        }
        if (!coordinator_.initialize(&error_)) return;
        source_ = std::make_unique<VideoWallpaperSource>(
            video.string(), "fixture", &coordinator_
        );
        usable_ = source_->prepare(&error_);
        if (usable_) {
            source_->start(std::chrono::steady_clock::now());
            pump(*source_);
        }
    }

    [[nodiscard]] bool usable() const noexcept { return usable_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::string& skip_reason() const noexcept {
        return skip_reason_;
    }

    VideoWallpaperSource& source() noexcept { return *source_; }

private:
    VideoDecodeCoordinator coordinator_;
    std::unique_ptr<VideoWallpaperSource> source_;
    std::string error_;
    std::string skip_reason_;
    bool usable_ = false;
};

TEST(VideoWallpaperSourceTest, DecodesPresentsAndLoopsAfterStart) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    HeadlessGl gl;
    ASSERT_TRUE(gl.ready());
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(build_fixture(video, "testsrc=size=64x64:rate=10:duration=0.8"));

    PlayingSource playing(video);
    ASSERT_TRUE(playing.usable()) << playing.error();
    auto& source = playing.source();
    EXPECT_TRUE(source.started());
    EXPECT_TRUE(source.current_frame().valid());
    EXPECT_TRUE(source.committable());
    EXPECT_FALSE(source.failed());

    const auto decoded_at_start = source.frames_decoded();
    drive(source, std::chrono::milliseconds(400));
    EXPECT_GE(source.frames_decoded(), decoded_at_start + 3);
    EXPECT_FALSE(source.failed());

    // The fixture is 0.8 s long: a wallpaper that stops at end of stream instead
    // of looping is a dead wallpaper.
    const auto loops_before = source.loop_restarts();
    drive(source, std::chrono::milliseconds(1500));
    EXPECT_GT(source.loop_restarts(), loops_before);
    EXPECT_FALSE(source.failed());

    source.stop();
    EXPECT_FALSE(source.started());
}

// BUG-1: applying the wallpaper that is already playing.
//
// The source registry returns the same object for the same path, so a re-apply
// reaches prepare() on a source whose decode worker is running. prepare() used
// to call VideoDecoder::open() unconditionally, and open() closes first: that
// freed the FFmpeg contexts the worker was still decoding with. The renderer
// process died, the controller fell back to GTK, and the screen went blank.
TEST(VideoWallpaperSourceTest, ReApplyingAPlayingWallpaperKeepsItPlaying) {
    const HangGuard hung_after(std::chrono::seconds(15));
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    HeadlessGl gl;
    ASSERT_TRUE(gl.ready());
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(build_fixture(video, "testsrc=size=64x64:rate=10:duration=0.8"));

    PlayingSource playing(video);
    ASSERT_TRUE(playing.usable()) << playing.error();
    auto& source = playing.source();
    drive(source, std::chrono::milliseconds(300));

    // The re-apply. It must be a no-op, not a re-open of live FFmpeg state.
    const auto decoded_before = source.frames_decoded();
    std::string error;
    EXPECT_TRUE(source.prepare(&error)) << error;
    EXPECT_TRUE(source.decoder_open());
    EXPECT_TRUE(source.started());
    EXPECT_TRUE(source.committable());
    EXPECT_TRUE(source.current_frame().valid());

    // Playback continues from the same timeline: no frozen source, no lost
    // worker, no discarded queue.
    drive(source, std::chrono::milliseconds(400));
    EXPECT_GT(source.frames_decoded(), decoded_before);
    EXPECT_FALSE(source.failed());
    EXPECT_TRUE(source.current_frame().valid());

    source.stop();
}

// Repeated re-applies must be as harmless as the first one: this is what a user
// clicking the same tile in the carousel produces.
TEST(VideoWallpaperSourceTest, ReApplyingManyTimesKeepsDecoding) {
    const HangGuard hung_after(std::chrono::seconds(15));
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    HeadlessGl gl;
    ASSERT_TRUE(gl.ready());
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(build_fixture(video, "testsrc=size=64x64:rate=10:duration=0.8"));

    PlayingSource playing(video);
    ASSERT_TRUE(playing.usable()) << playing.error();
    auto& source = playing.source();

    std::string error;
    for (int attempt = 0; attempt < 5; ++attempt) {
        drive(source, std::chrono::milliseconds(60));
        ASSERT_TRUE(source.prepare(&error)) << error;
        ASSERT_TRUE(source.decoder_open());
    }

    drive(source, std::chrono::milliseconds(400));
    EXPECT_FALSE(source.failed());
    EXPECT_TRUE(source.current_frame().valid());
    source.stop();
}

// prepare() before start() is a decode, not a playback, and repeating it is
// safe because no worker is running yet. This is the case the re-apply guard
// must not swallow: a fresh prepare still has to open the decoder.
TEST(VideoWallpaperSourceTest, PreparingTwiceBeforeStartStillOpensTheDecoder) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    HeadlessGl gl;
    ASSERT_TRUE(gl.ready());
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(build_fixture(video, "testsrc=size=64x64:rate=10:duration=0.8"));

    VideoDecodeCoordinator coordinator;
    std::string initialize_error;
    ASSERT_TRUE(coordinator.initialize(&initialize_error)) << initialize_error;

    VideoWallpaperSource source(video.string(), "fixture", &coordinator);
    std::string error;
    ASSERT_TRUE(source.prepare(&error)) << error;
    ASSERT_TRUE(source.decoder_open());
    ASSERT_TRUE(source.prepare(&error)) << error;
    ASSERT_TRUE(source.decoder_open());
    ASSERT_FALSE(source.started());

    source.start(std::chrono::steady_clock::now());
    pump(source);
    EXPECT_TRUE(source.current_frame().valid());
    source.stop();
}

// stop() closed the frame queue and start() used to leave it closed, so a source
// that stopped and started again accepted no frames and silently produced
// nothing (the second half of BUG-1).
TEST(VideoWallpaperSourceTest, AStoppedSourceRestartsAndKeepsDecoding) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    HeadlessGl gl;
    ASSERT_TRUE(gl.ready());
    ScopedTempDirectory directory;
    const auto video = directory.path() / "wallpaper.mp4";
    ASSERT_TRUE(build_fixture(video, "testsrc=size=64x64:rate=10:duration=0.8"));

    PlayingSource playing(video);
    ASSERT_TRUE(playing.usable()) << playing.error();
    auto& source = playing.source();
    drive(source, std::chrono::milliseconds(300));

    source.stop();
    // A stopped source has no timeline and no timed work.
    EXPECT_EQ(source.queue_size(), 0U);
    EXPECT_EQ(source.next_deadline(), std::nullopt);
    EXPECT_FALSE(source.started());

    const auto decoded_before = source.frames_decoded();
    source.start(std::chrono::steady_clock::now());
    EXPECT_TRUE(source.started());
    pump(source);
    EXPECT_TRUE(source.current_frame().valid());

    // The reopened queue takes frames again: without this the restarted source
    // would sit on its last frame and never advance.
    drive(source, std::chrono::milliseconds(400));
    EXPECT_GT(source.frames_decoded(), decoded_before);
    EXPECT_FALSE(source.failed());
    source.stop();
}

// A video source that cannot be prepared at all must say so, not hang or crash.
TEST(VideoWallpaperSourceTest, PreparingAnUnreadableVideoReportsWhy) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    VideoDecodeCoordinator coordinator;
    std::string initialize_error;
    ASSERT_TRUE(coordinator.initialize(&initialize_error)) << initialize_error;

    VideoWallpaperSource source(
        "/nonexistent/realmheart/wallpaper.mp4", "fixture", &coordinator
    );
    std::string error;
    EXPECT_FALSE(source.prepare(&error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(source.committable());
    EXPECT_FALSE(source.started());
}

#else

TEST(VideoWallpaperSourceTest, ReportsThatLivePlaybackIsNotCompiledIn) {
    VideoWallpaperSource source("/any/wallpaper.mp4", "fixture", nullptr);
    std::string error;
    EXPECT_FALSE(source.prepare(&error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(source.committable());
}

#endif
