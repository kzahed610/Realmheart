#include "wallpaper-native/gpu/CpuUploadFrameImporter.hpp"
#include "wallpaper-native/video/VideoFrame.hpp"

#include <gtest/gtest.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cstring>
#include <string>
#include <vector>

using realmheart::wallpaper_native::CpuUploadFrameImporter;
using realmheart::wallpaper_native::RenderFrame;
using realmheart::wallpaper_native::VideoFrame;
using realmheart::wallpaper_native::VideoFrameFormat;

namespace {

VideoFrame rgb_frame(int width, int height, unsigned char value) {
    VideoFrame frame;
    frame.format = VideoFrameFormat::Rgba8;
    frame.width = width;
    frame.height = height;
    frame.pixels.assign(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U,
        value
    );
    return frame;
}

// A minimal GL environment: EGL provides a headless context through Mesa's
// software renderer, which is enough to exercise the upload rules.
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
        constexpr EGLint surface_attributes[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        surface_ = eglCreatePbufferSurface(
            display_, config_, surface_attributes
        );
        if (surface_ == EGL_NO_SURFACE) return;
        constexpr EGLint context_attributes[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
        };
        context_ = eglCreateContext(
            display_, config_, EGL_NO_CONTEXT, context_attributes
        );
        if (context_ != EGL_NO_CONTEXT) {
            eglMakeCurrent(display_, surface_, surface_, context_);
        }
    }

    ~HeadlessGl() {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context_ != EGL_NO_CONTEXT) {
                eglDestroyContext(display_, context_);
            }
            if (surface_ != EGL_NO_SURFACE) {
                eglDestroySurface(display_, surface_);
            }
            eglTerminate(display_);
        }
    }

    [[nodiscard]] bool ready() const noexcept {
        return display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT;
    }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
};

class CpuUploadFrameImporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(gl_.ready()) << "no headless GL context available";
    }

    HeadlessGl gl_;
    CpuUploadFrameImporter importer_;
};

TEST_F(CpuUploadFrameImporterTest, FirstUploadAllocatesATexture) {
    RenderFrame frame;
    const VideoFrame source = rgb_frame(8, 8, 0x40);
    std::string error;
    ASSERT_TRUE(importer_.update(frame, source, &error)) << error;
    EXPECT_NE(frame.texture, 0U);
    EXPECT_EQ(frame.width, 8);
    EXPECT_EQ(frame.height, 8);
    EXPECT_EQ(importer_.allocations(), 1U);
    EXPECT_EQ(importer_.uploads(), 1U);
}

// The rule that keeps long runtimes bounded: same geometry must overwrite, not
// allocate. A regression here is invisible until memory climbs.
TEST_F(CpuUploadFrameImporterTest, SameGeometryReusesTheTexture) {
    RenderFrame frame;
    const GLuint first = [&] {
        std::string error;
        EXPECT_TRUE(importer_.update(frame, rgb_frame(16, 16, 1), &error));
        return frame.texture;
    }();

    for (int index = 2; index < 100; ++index) {
        std::string error;
        ASSERT_TRUE(importer_.update(frame, rgb_frame(16, 16, index), &error)) << error;
        ASSERT_EQ(frame.texture, first);
    }
    EXPECT_EQ(importer_.allocations(), 1U);
    EXPECT_EQ(importer_.uploads(), 99U);
    EXPECT_EQ(frame.width, 16);
    EXPECT_EQ(frame.height, 16);
}

TEST_F(CpuUploadFrameImporterTest, GeometryChangeReallocates) {
    RenderFrame frame;
    std::string error;
    ASSERT_TRUE(importer_.update(frame, rgb_frame(8, 8, 1), &error));

    ASSERT_TRUE(importer_.update(frame, rgb_frame(16, 16, 2), &error));
    EXPECT_NE(frame.texture, 0U);
    EXPECT_EQ(frame.width, 16);
    EXPECT_EQ(frame.height, 16);
    // A new geometry means a new allocation. (The texture name is deliberately
    // not compared: a driver is free to hand back the same name after a
    // delete, which would make that assertion machine-dependent.)
    EXPECT_EQ(importer_.allocations(), 2U);
    EXPECT_EQ(importer_.uploads(), 2U);
}

TEST_F(CpuUploadFrameImporterTest, OddWidthRowsAreNotMisaligned) {
    // Width * 4 is always 4-aligned, but a width that is not a power of two
    // must not corrupt the upload.
    RenderFrame frame;
    const VideoFrame source = rgb_frame(7, 5, 0x7f);
    std::string error;
    ASSERT_TRUE(importer_.update(frame, source, &error)) << error;
    EXPECT_EQ(frame.width, 7);
    EXPECT_EQ(frame.height, 5);
    while (glGetError() != GL_NO_ERROR) {}
}

TEST_F(CpuUploadFrameImporterTest, RejectsAnIncompleteFrame) {
    RenderFrame frame;
    VideoFrame broken = rgb_frame(4, 4, 1);
    broken.pixels.resize(4);  // size no longer matches the geometry
    std::string error;
    EXPECT_FALSE(importer_.update(frame, broken, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(frame.texture, 0U);
}

TEST_F(CpuUploadFrameImporterTest, ReleaseDropsTheTexture) {
    RenderFrame frame;
    std::string error;
    ASSERT_TRUE(importer_.update(frame, rgb_frame(4, 4, 1), &error));
    importer_.release(frame);
    EXPECT_EQ(frame.texture, 0U);
    EXPECT_EQ(frame.width, 0);
    EXPECT_EQ(frame.height, 0);
}

TEST_F(CpuUploadFrameImporterTest, UploadFailsCleanlyAfterRelease) {
    RenderFrame frame;
    std::string error;
    ASSERT_TRUE(importer_.update(frame, rgb_frame(4, 4, 1), &error));
    importer_.release(frame);
    ASSERT_TRUE(importer_.update(frame, rgb_frame(4, 4, 2), &error));
    EXPECT_NE(frame.texture, 0U);
}

} // namespace
