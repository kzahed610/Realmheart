#include "wallpaper-native/gpu/CpuUploadFrameImporter.hpp"
#include "wallpaper-native/gpu/RenderFrame.hpp"
#include "wallpaper-native/gpu/WallpaperShaders.hpp"
#include "wallpaper-native/gpu/YuvColorimetry.hpp"
#include "wallpaper-native/video/VideoDecoder.hpp"

#include <gtest/gtest.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using realmheart::wallpaper_native::CpuUploadFrameImporter;
using realmheart::wallpaper_native::RenderFrame;
using realmheart::wallpaper_native::RenderFrameKind;
using realmheart::wallpaper_native::RenderFrameView;
using realmheart::wallpaper_native::VideoColorRange;
using realmheart::wallpaper_native::VideoColorSpace;
using realmheart::wallpaper_native::VideoDecoder;
using realmheart::wallpaper_native::VideoFrame;
using realmheart::wallpaper_native::VideoFrameFormat;
using realmheart::wallpaper_native::WallpaperShaders;
using realmheart::wallpaper_native::yuv_conversion_for;

namespace {

constexpr int kSurface = 64;

// A GL environment big enough to read pixels back out of. EGL provides one
// headlessly through Mesa's software renderer.
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
            EGL_WIDTH, kSurface, EGL_HEIGHT, kSurface, EGL_NONE
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

    // Renders 'frame' full-screen and returns the RGBA pixels.
    std::array<unsigned char, 4> render_center(
        const WallpaperShaders& shaders,
        const RenderFrame& frame
    ) {
        glViewport(0, 0, kSurface, kSurface);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        shaders.draw(
            RenderFrameView{&frame},
            RenderFrameView{},
            std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F},
            std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F},
            1.0F
        );
        std::array<unsigned char, 4> pixel{};
        glReadPixels(
            kSurface / 2, kSurface / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
            pixel.data()
        );
        return pixel;
    }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    bool ready_ = false;
};

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        std::string pattern = "/tmp/realmheart-yuv-frame-XXXXXX";
        const char* created = ::mkdtemp(pattern.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = created;
    }
    ~ScopedTempDirectory() { std::filesystem::remove_all(root_); }
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

[[maybe_unused]] bool ffmpeg_available() {
    return std::system("command -v ffmpeg >/dev/null 2>&1") == 0;
}

// A single flat colour at full luma and chroma, so the conversion has exactly
// one answer to get right.
VideoFrame nv12_frame(
    int width,
    int height,
    unsigned char luma,
    unsigned char chroma_u,
    unsigned char chroma_v,
    VideoColorSpace color_space,
    VideoColorRange color_range
) {
    VideoFrame frame;
    frame.format = VideoFrameFormat::Nv12;
    frame.width = width;
    frame.height = height;
    frame.color_space = color_space;
    frame.color_range = color_range;
    const auto luma_bytes = static_cast<std::size_t>(width) * height;
    frame.pixels.assign(luma_bytes + luma_bytes / 2, luma);
    for (std::size_t index = luma_bytes; index < frame.pixels.size(); index += 2) {
        frame.pixels[index] = chroma_u;
        frame.pixels[index + 1] = chroma_v;
    }
    return frame;
}

VideoFrame rgba_frame(int width, int height, unsigned char r, unsigned char g, unsigned char b) {
    VideoFrame frame;
    frame.format = VideoFrameFormat::Rgba8;
    frame.width = width;
    frame.height = height;
    const auto count = static_cast<std::size_t>(width) * height;
    frame.pixels.reserve(count * 4U);
    for (std::size_t index = 0; index < count; ++index) {
        frame.pixels.push_back(r);
        frame.pixels.push_back(g);
        frame.pixels.push_back(b);
        frame.pixels.push_back(255);
    }
    return frame;
}

int channel_distance(const std::array<unsigned char, 4>& left,
                     const std::array<unsigned char, 4>& right) {
    int worst = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        worst = std::max(
            worst, std::abs(static_cast<int>(left[index]) - static_cast<int>(right[index]))
        );
    }
    return worst;
}

} // namespace

// The matrix is the whole colour story, so its coefficients are checked against
// the ITU-R values rather than trusted.
TEST(YuvColorimetryTest, LimitedRangeBt601LiftsLumaAndClampsMidChroma) {
    const auto conversion = yuv_conversion_for(
        VideoColorSpace::Bt601, VideoColorRange::Limited
    );
    EXPECT_NEAR(conversion.matrix[0], 255.0F / 219.0F, 1e-4F);
    EXPECT_NEAR(conversion.luma_offset, -16.0F / 255.0F, 1e-6F);
    EXPECT_NEAR(conversion.matrix[2], 1.402, 0.01F);   // red from Cr
    EXPECT_NEAR(conversion.matrix[7], 1.772, 0.01F);   // blue from Cb
    EXPECT_NEAR(conversion.matrix[4], -0.344, 0.01F);  // green from Cb
    EXPECT_NEAR(conversion.matrix[5], -0.714, 0.01F);  // green from Cr
}

TEST(YuvColorimetryTest, Bt709UsesItsOwnPrimaries) {
    const auto bt601 = yuv_conversion_for(
        VideoColorSpace::Bt601, VideoColorRange::Limited
    );
    const auto bt709 = yuv_conversion_for(
        VideoColorSpace::Bt709, VideoColorRange::Limited
    );
    EXPECT_NEAR(bt709.matrix[2], 1.5748, 0.01F);
    EXPECT_NEAR(bt709.matrix[7], 1.8556, 0.01F);
    // The primaries differ, so the matrices must not be the same.
    EXPECT_GT(std::abs(bt709.matrix[2] - bt601.matrix[2]), 0.05F);
    EXPECT_GT(std::abs(bt709.matrix[7] - bt601.matrix[7]), 0.05F);
}

TEST(YuvColorimetryTest, FullRangeNeedsNoLift) {
    const auto conversion = yuv_conversion_for(
        VideoColorSpace::Bt601, VideoColorRange::Full
    );
    EXPECT_FLOAT_EQ(conversion.luma_offset, 0.0F);
    EXPECT_FLOAT_EQ(conversion.matrix[0], 1.0F);
}

#if REALMHEART_HAVE_LIVE_WALLPAPER

// The contract that matters: the frame that reaches the screen must be the same
// colour it was when the renderer converted to RGBA on the CPU. A graphics
// grey must not come out tinted, and saturated primaries must not shift.
TEST(YuvFrameTest, YuvSamplingMatchesTheRgbPathForRealDecodedFrames) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    HeadlessGl gl;
    ASSERT_TRUE(gl.ready());
    ScopedTempDirectory directory;
    const auto video = directory.root() / "wallpaper.mp4";
    const std::string command =
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i "
        "\"testsrc=size=64x64:rate=10:duration=0.5\" -c:v libx264 "
        "-pix_fmt yuv420p -frames:v 5 -f mp4 " + video.string() + " 2>/dev/null";
    ASSERT_EQ(std::system(command.c_str()), 0);

    VideoDecoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.open(video.string(), &error)) << error;
    VideoFrame frame;
    ASSERT_TRUE(decoder.decode_next(frame, &error)) << error;
    ASSERT_TRUE(frame.valid());
    ASSERT_EQ(frame.format, VideoFrameFormat::Nv12);
    decoder.close();

    CpuUploadFrameImporter importer;
    WallpaperShaders shaders;
    ASSERT_TRUE(shaders.initialize(&error)) << error;

    // The same conversion the shader does, in normalized units, computed here so
    // the GL output has an independent witness.
    //
    // Chroma is sampled exactly the way the hardware samples it: the chroma
    // plane is half resolution, so the same normalized coordinate lands half a
    // chroma texel further along and GL_LINEAR blends two of them.
    auto bilinear = [&frame](auto read, float normal_x, float normal_y, bool chroma) {
        const int plane_width = chroma ? frame.width / 2 : frame.width;
        const int plane_height = chroma ? frame.height / 2 : frame.height;
        const float texel_x = normal_x * plane_width - 0.5F;
        const float texel_y = normal_y * plane_height - 0.5F;
        const int left = std::clamp(static_cast<int>(texel_x), 0, plane_width - 1);
        const int bottom = std::clamp(static_cast<int>(texel_y), 0, plane_height - 1);
        const int right = std::min(left + 1, plane_width - 1);
        const int top = std::min(bottom + 1, plane_height - 1);
        const float blend_x = std::clamp(texel_x - left, 0.0F, 1.0F);
        const float blend_y = std::clamp(texel_y - bottom, 0.0F, 1.0F);
        const auto at = [&](int x, int y) { return read(x, y); };
        return (at(left, bottom) * (1.0F - blend_x) +
                at(right, bottom) * blend_x) * (1.0F - blend_y) +
               (at(left, top) * (1.0F - blend_x) + at(right, top) * blend_x) *
                   blend_y;
    };

    auto reference = [&](std::size_t x, std::size_t y) {
        const auto conversion = yuv_conversion_for(
            frame.color_space, frame.color_range
        );
        // The centre of the pixel, in normalized coordinates, which is what the
        // rasterizer hands the fragment shader.
        const float normal_x = (static_cast<float>(x) + 0.5F) /
                               static_cast<float>(frame.width);
        const float normal_y = (static_cast<float>(y) + 0.5F) /
                               static_cast<float>(frame.height);
        const auto luma_at = [&frame](int px, int py) {
            return static_cast<float>(
                frame.pixels[static_cast<std::size_t>(py) * frame.width + px]
            );
        };
        const auto chroma_at = [&frame](int px, int py, std::size_t component) {
            const std::size_t index = frame.plane_offset() +
                                      static_cast<std::size_t>(py) * frame.width +
                                      static_cast<std::size_t>(px) * 2 + component;
            return static_cast<float>(frame.pixels[index]);
        };
        const float luma_value = bilinear(
            luma_at, normal_x, normal_y, false
        );
        const float u_value = bilinear(
            [&](int px, int py) { return chroma_at(px, py, 0); },
            normal_x, normal_y, true
        );
        const float v_value = bilinear(
            [&](int px, int py) { return chroma_at(px, py, 1); },
            normal_x, normal_y, true
        );
        const float luma_f = luma_value / 255.0F + conversion.luma_offset;
        const float u = u_value / 255.0F - 0.5F;
        const float v = v_value / 255.0F - 0.5F;
        const std::array<float, 3> rgb{
            conversion.matrix[0] * luma_f + conversion.matrix[2] * v,
            conversion.matrix[3] * luma_f + conversion.matrix[4] * u +
                conversion.matrix[5] * v,
            conversion.matrix[6] * luma_f + conversion.matrix[7] * u
        };
        std::array<unsigned char, 4> out{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            out[channel] = static_cast<unsigned char>(std::clamp(
                std::lround(rgb[channel] * 255.0F), 0L, 255L
            ));
        }
        out[3] = 255;
        return out;
    };

    RenderFrame uploaded;
    ASSERT_TRUE(importer.update(uploaded, frame, &error)) << error;
    EXPECT_EQ(uploaded.kind, RenderFrameKind::Nv12Planes);
    EXPECT_NE(uploaded.chroma_texture, 0U);
    EXPECT_EQ(uploaded.width, frame.width);
    EXPECT_EQ(uploaded.height, frame.height);

    // The whole surface covers the frame, so the centre pixel is the centre of
    // the frame: one probe is enough to catch a wrong matrix or a swapped
    // chroma pair.
    const auto rendered = gl.render_center(shaders, uploaded);
    // GL reads pixels bottom-up, so the surface's centre row is the frame's
    // second-to-last row.
    const auto expected = reference(frame.width / 2, frame.height - 1 - frame.height / 2);
    EXPECT_LE(channel_distance(rendered, expected), 3)
        << "YUV sampling produced a colour the frame does not contain";

    // A second upload of the same frame must reuse the textures rather than
    // allocating new ones.
    const auto allocations = importer.allocations();
    ASSERT_TRUE(importer.update(uploaded, frame, &error)) << error;
    EXPECT_EQ(importer.allocations(), allocations);
    EXPECT_EQ(importer.uploads(), 2U);

    importer.release(uploaded);
    EXPECT_EQ(uploaded.texture, 0U);
    EXPECT_EQ(uploaded.chroma_texture, 0U);
    shaders.release();
}

// An RGB frame and a YUV frame must both be drawable through the same program,
// and a transition between them must not produce a third colour.
TEST(YuvFrameTest, RgbAndYuvFramesShareOneProgram) {
    if (!ffmpeg_available()) GTEST_SKIP() << "ffmpeg unavailable";
    HeadlessGl gl;
    ASSERT_TRUE(gl.ready());
    CpuUploadFrameImporter importer;
    WallpaperShaders shaders;
    std::string error;
    ASSERT_TRUE(shaders.initialize(&error)) << error;

    // A mid grey in RGB, and the same grey as limited-range BT.601 YUV
    // (luma 126, chroma 128).
    const auto rgb = rgba_frame(64, 64, 128, 128, 128);
    const auto yuv = nv12_frame(
        64, 64, 126, 128, 128, VideoColorSpace::Bt601, VideoColorRange::Limited
    );

    RenderFrame rgb_frame;
    RenderFrame yuv_frame;
    ASSERT_TRUE(importer.update(rgb_frame, rgb, &error)) << error;
    ASSERT_TRUE(importer.update(yuv_frame, yuv, &error)) << error;
    ASSERT_EQ(rgb_frame.kind, RenderFrameKind::RgbTexture);
    ASSERT_EQ(yuv_frame.kind, RenderFrameKind::Nv12Planes);

    const auto from_rgb = gl.render_center(shaders, rgb_frame);
    const auto from_yuv = gl.render_center(shaders, yuv_frame);
    EXPECT_LE(channel_distance(from_rgb, from_yuv), 4)
        << "the same grey came out differently through the two frame kinds";

    importer.release(rgb_frame);
    importer.release(yuv_frame);
    shaders.release();
}

// A geometry change has to reallocate both planes; a same-size frame must not.
TEST(YuvFrameTest, GeometryChangeReallocatesPlanes) {
    HeadlessGl gl;
    ASSERT_TRUE(gl.ready());
    CpuUploadFrameImporter importer;
    std::string error;

    RenderFrame frame;
    const auto small = nv12_frame(
        32, 32, 200, 64, 192, VideoColorSpace::Bt601, VideoColorRange::Limited
    );
    ASSERT_TRUE(importer.update(frame, small, &error)) << error;
    EXPECT_EQ(importer.allocations(), 1U);
    EXPECT_EQ(frame.width, 32);
    EXPECT_EQ(frame.height, 32);

    ASSERT_TRUE(importer.update(frame, small, &error)) << error;
    EXPECT_EQ(importer.allocations(), 1U);
    EXPECT_EQ(importer.uploads(), 2U);

    const auto large = nv12_frame(
        64, 48, 200, 64, 192, VideoColorSpace::Bt601, VideoColorRange::Limited
    );
    ASSERT_TRUE(importer.update(frame, large, &error)) << error;
    EXPECT_EQ(importer.allocations(), 2U);
    EXPECT_EQ(frame.width, 64);
    EXPECT_EQ(frame.height, 48);

    // A frame in the other layout is a different frame, not a new geometry.
    const auto rgb = rgba_frame(64, 48, 10, 20, 30);
    ASSERT_TRUE(importer.update(frame, rgb, &error)) << error;
    EXPECT_EQ(frame.kind, RenderFrameKind::RgbTexture);
    EXPECT_EQ(importer.allocations(), 3U);

    importer.release(frame);
}

#else

TEST(YuvFrameTest, ReportsThatYuvSamplingNeedsLiveSupport) {
    const auto conversion = yuv_conversion_for(
        VideoColorSpace::Bt601, VideoColorRange::Limited
    );
    // The matrix exists in every build; only the decoded frames do not.
    EXPECT_NEAR(conversion.matrix[0], 255.0F / 219.0F, 1e-4F);
}

#endif
