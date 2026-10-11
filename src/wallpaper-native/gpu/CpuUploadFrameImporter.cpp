#include "wallpaper-native/gpu/CpuUploadFrameImporter.hpp"

#include <GLES2/gl2.h>

namespace realmheart::wallpaper_native {

bool CpuUploadFrameImporter::update(
    RenderFrame& frame,
    const VideoFrame& source,
    std::string* error_message
) {
    if (!source.valid()) {
        if (error_message != nullptr) {
            *error_message = "the decoded video frame is incomplete";
        }
        return false;
    }

    const bool needs_allocation = frame.texture == 0 ||
                                  frame.width != source.width ||
                                  frame.height != source.height;
    if (needs_allocation) {
        if (frame.texture != 0) {
            glDeleteTextures(1, &frame.texture);
            frame.texture = 0;
        }
        // Clear any stale error from driver setup before attributing a failure
        // to this upload.
        while (glGetError() != GL_NO_ERROR) {}

        glGenTextures(1, &frame.texture);
        if (frame.texture == 0) {
            if (error_message != nullptr) {
                *error_message = "OpenGL failed to allocate a video wallpaper texture";
            }
            return false;
        }
        frame.width = source.width;
        frame.height = source.height;
        ++allocations_;
    }

    glBindTexture(GL_TEXTURE_2D, frame.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (needs_allocation) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            source.width,
            source.height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            source.pixels.data()
        );
    } else {
        // Same geometry: overwrite in place instead of re-allocating.
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            source.width,
            source.height,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            source.pixels.data()
        );
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    if (glGetError() != GL_NO_ERROR) {
        if (error_message != nullptr) {
            *error_message = "OpenGL failed while uploading a video wallpaper frame";
        }
        return false;
    }
    ++uploads_;
    return true;
}

void CpuUploadFrameImporter::release(RenderFrame& frame) noexcept {
    if (frame.texture != 0) {
        glDeleteTextures(1, &frame.texture);
    }
    frame = {};
}

} // namespace realmheart::wallpaper_native
