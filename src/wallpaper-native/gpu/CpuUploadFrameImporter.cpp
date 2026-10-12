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

    // Storage is allocated only when the geometry or the pixel layout changes.
    // Everything else overwrites in place, so a long-running wallpaper never
    // re-allocates GPU memory per decoded frame (renderer plan phase 6).
    const bool layout_changed =
        frame.texture == 0 || frame.width != source.width ||
        frame.height != source.height ||
        frame.kind != render_frame_kind_for(source.format);
    if (layout_changed) {
        release(frame);
        // Clear any stale error from driver setup before attributing a failure
        // to this upload.
        while (glGetError() != GL_NO_ERROR) {}

        glGenTextures(1, &frame.texture);
        if (frame.texture == 0) {
            if (error_message != nullptr) {
                *error_message =
                    "OpenGL failed to allocate a video wallpaper texture";
            }
            return false;
        }
        if (source.format == VideoFrameFormat::Nv12) {
            glGenTextures(1, &frame.chroma_texture);
            if (frame.chroma_texture == 0) {
                glDeleteTextures(1, &frame.texture);
                frame.texture = 0;
                if (error_message != nullptr) {
                    *error_message =
                        "OpenGL failed to allocate a video wallpaper chroma texture";
                }
                return false;
            }
            frame.kind = RenderFrameKind::Nv12Planes;
        } else {
            frame.kind = RenderFrameKind::RgbTexture;
        }
        frame.width = source.width;
        frame.height = source.height;
        ++allocations_;
    }

    // The colourimetry is a property of the frame's pixels, so it travels with
    // them. An RGB frame never reads it.
    frame.color_space = source.color_space;
    frame.color_range = source.color_range;

    const GLint filter = GL_LINEAR;
    const GLint wrap = GL_CLAMP_TO_EDGE;
    if (frame.kind == RenderFrameKind::Nv12Planes) {
        // Luma: one byte per pixel. GL_LUMINANCE replicates it to RGB, so the
        // shader reads it from .r.
        glBindTexture(GL_TEXTURE_2D, frame.texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        if (layout_changed) {
            glTexImage2D(
                GL_TEXTURE_2D, 0, GL_LUMINANCE, source.width, source.height, 0,
                GL_LUMINANCE, GL_UNSIGNED_BYTE, source.pixels.data()
            );
        } else {
            glTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, 0, source.width, source.height,
                GL_LUMINANCE, GL_UNSIGNED_BYTE, source.pixels.data()
            );
        }

        // Chroma: interleaved UV at half resolution. GL_LUMINANCE_ALPHA hands
        // the shader the pair through .ra, so an extra de-interleave pass never
        // happens on the CPU.
        const int chroma_width = source.width / 2;
        const int chroma_height = source.height / 2;
        const unsigned char* chroma_pixels = source.pixels.data() +
                                             source.plane_offset();
        glBindTexture(GL_TEXTURE_2D, frame.chroma_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        if (layout_changed) {
            glTexImage2D(
                GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA,
                chroma_width, chroma_height, 0,
                GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, chroma_pixels
            );
        } else {
            glTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, 0, chroma_width, chroma_height,
                GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, chroma_pixels
            );
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        if (glGetError() != GL_NO_ERROR) {
            if (error_message != nullptr) {
                *error_message =
                    "OpenGL failed while uploading a video wallpaper frame";
            }
            return false;
        }
        ++uploads_;
        return true;
    }

    glBindTexture(GL_TEXTURE_2D, frame.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    if (layout_changed) {
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA, source.width, source.height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, source.pixels.data()
        );
    } else {
        glTexSubImage2D(
            GL_TEXTURE_2D, 0, 0, 0, source.width, source.height,
            GL_RGBA, GL_UNSIGNED_BYTE, source.pixels.data()
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
    if (frame.chroma_texture != 0) {
        glDeleteTextures(1, &frame.chroma_texture);
    }
    if (frame.texture != 0) {
        glDeleteTextures(1, &frame.texture);
    }
    frame = {};
}

} // namespace realmheart::wallpaper_native
