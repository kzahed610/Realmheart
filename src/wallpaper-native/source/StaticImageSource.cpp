#include "wallpaper-native/source/StaticImageSource.hpp"

#include "wallpaper-native/NativeWallpaperContracts.hpp"
#include "wallpaper-native/source/StaticDecodePlan.hpp"

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <utility>

namespace realmheart::wallpaper_native {

StaticImageSource::StaticImageSource(
    std::string path,
    std::shared_ptr<const std::string> bytes
)
    : path_(std::move(path)), bytes_(std::move(bytes)) {}

StaticImageSource::~StaticImageSource() {
    release_frame();
}

void StaticImageSource::release_frame() noexcept {
    if (frame_.texture != 0) {
        glDeleteTextures(1, &frame_.texture);
    }
    frame_ = {};
}

bool StaticImageSource::prepare(std::string* error_message) {
    // A source is prepared once per transaction. Re-preparing replaces the
    // frame, so a failed re-decode cannot leave a half-decoded frame behind.
    release_frame();
    return bytes_ != nullptr ? decode_from_bytes(error_message)
                             : decode_from_file(error_message);
}

void StaticImageSource::start(std::chrono::steady_clock::time_point) noexcept {}

void StaticImageSource::stop() noexcept {}

std::optional<std::chrono::steady_clock::time_point>
StaticImageSource::next_deadline() const noexcept {
    // Static images have no timeline: no deadline, no periodic work.
    return std::nullopt;
}

SourceTickResult StaticImageSource::tick(std::chrono::steady_clock::time_point) noexcept {
    // Nothing to advance; the renderer never has to wake for a static source.
    return {};
}

RenderFrameView StaticImageSource::current_frame() const noexcept {
    return RenderFrameView{frame_.texture != 0 ? &frame_ : nullptr};
}

void StaticImageSource::set_output_requirement(
    int width,
    int height
) noexcept {
    required_width_ = width;
    required_height_ = height;
}

bool StaticImageSource::requires_topology_rederive(
    int required_width,
    int required_height
) const noexcept {
    return native_output_requires_redecode(
        required_width_,
        required_height_,
        required_width,
        required_height
    );
}

bool StaticImageSource::adapt_to_topology(
    int required_width,
    int required_height,
    std::string* error_message
) {
    // Decode geometry is source-native, so a larger output box never needs a
    // bigger decode; but the legacy contract re-decodes when the required box
    // grows, and a source must stay able to answer that for hotplug/scale
    // increases. Keep the same decision rule rather than changing behavior.
    //
    // A re-decode replaces the frame in place. A failed *draw* after a
    // successful re-decode cannot restore the previous frame here, but the
    // renderer treats any draw failure as fatal, so no visible state survives
    // it either way.
    if (!requires_topology_rederive(required_width, required_height)) return true;

    if (bytes_ != nullptr) {
        if (bytes_->empty() || bytes_->size() > kNativeMaxSourceBytes) {
            if (error_message != nullptr) {
                *error_message =
                    "owned wallpaper bytes exceed the decode budget or are empty";
            }
            return false;
        }
        if (!decode_from_bytes(error_message)) return false;
    } else if (!decode_from_file(error_message)) {
        return false;
    }

    required_width_ = required_width;
    required_height_ = required_height;
    return true;
}

bool StaticImageSource::decode_from_file(std::string* error_message) {
    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(path_, file_error);
    if (file_error || file_size > kNativeMaxSourceBytes) {
        if (error_message != nullptr) {
            *error_message = "wallpaper file exceeds the decode budget";
        }
        return false;
    }

    int source_width = 0;
    int source_height = 0;
    (void)gdk_pixbuf_get_file_info(path_.c_str(), &source_width, &source_height);

    GLint maximum_texture_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
    const StaticDecodePlan plan = plan_static_decode(
        source_width,
        source_height,
        maximum_texture_size
    );
    if (!plan.valid) {
        if (error_message != nullptr) {
            *error_message = "wallpaper dimensions exceed the decode budget";
        }
        return false;
    }

    GError* error = nullptr;
    GdkPixbuf* pixbuf = nullptr;
    if (plan.downscaled) {
        pixbuf = gdk_pixbuf_new_from_file_at_scale(
            path_.c_str(),
            plan.target_width,
            plan.target_height,
            TRUE,
            &error
        );
    } else {
        pixbuf = gdk_pixbuf_new_from_file(path_.c_str(), &error);
    }

    if (pixbuf == nullptr) {
        if (error_message != nullptr) {
            *error_message = error != nullptr
                ? error->message
                : "unable to decode wallpaper";
        }
        if (error != nullptr) g_error_free(error);
        return false;
    }
    if (error != nullptr) g_error_free(error);

    const bool uploaded = upload_pixels(
        pixbuf,
        "decoded wallpaper has an unsupported pixel layout",
        error_message
    );
    g_object_unref(pixbuf);
    return uploaded;
}

bool StaticImageSource::decode_from_bytes(std::string* error_message) {
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    if (loader == nullptr) {
        if (error_message != nullptr) {
            *error_message = "unable to create the wallpaper byte decoder";
        }
        return false;
    }

    GError* error = nullptr;
    const gboolean write_ok = gdk_pixbuf_loader_write(
        loader,
        reinterpret_cast<const guchar*>(bytes_->data()),
        bytes_->size(),
        &error
    );
    const gboolean close_ok = write_ok && gdk_pixbuf_loader_close(loader, &error);
    GdkPixbuf* pixbuf = nullptr;
    if (close_ok) {
        pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        if (pixbuf != nullptr) g_object_ref(pixbuf);
    }
    g_object_unref(loader);

    if (!write_ok || !close_ok || pixbuf == nullptr) {
        if (error_message != nullptr) {
            *error_message = error != nullptr
                ? error->message
                : "unable to decode owned wallpaper bytes";
        }
        g_clear_error(&error);
        if (pixbuf != nullptr) g_object_unref(pixbuf);
        return false;
    }
    g_clear_error(&error);

    const bool uploaded = upload_pixels(
        pixbuf,
        "decoded wallpaper exceeds the decode budget or has an unsupported pixel layout",
        error_message
    );
    g_object_unref(pixbuf);
    return uploaded;
}

bool StaticImageSource::upload_pixels(
    GdkPixbuf* pixbuf,
    const char* layout_error,
    std::string* error_message
) {
    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int row_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar* source = gdk_pixbuf_read_pixels(pixbuf);

    if (width <= 0 || height <= 0 || source == nullptr ||
        !plan_static_decode(width, height, 0).valid ||
        (channels != 3 && channels != 4)) {
        if (error_message != nullptr) *error_message = layout_error;
        return false;
    }

    const std::size_t tight_stride = static_cast<std::size_t>(width) * channels;
    const GLenum format = channels == 4 ? GL_RGBA : GL_RGB;

    // Clear any stale error left by driver initialization before attributing an
    // upload failure to this texture.
    while (glGetError() != GL_NO_ERROR) {}

    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0) {
        if (error_message != nullptr) {
            *error_message = "OpenGL failed to allocate a wallpaper texture";
        }
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (static_cast<std::size_t>(row_stride) == tight_stride) {
        // Common case: upload directly from the decoder buffer. Avoiding a
        // second full-image copy keeps wallpaper-change peak RAM bounded.
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            static_cast<GLint>(format),
            width,
            height,
            0,
            format,
            GL_UNSIGNED_BYTE,
            source
        );
    } else {
        // GLES2 has no GL_UNPACK_ROW_LENGTH. Allocate the texture once, then
        // upload padded pixbuf rows individually instead of copying the entire
        // image into another temporary buffer.
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            static_cast<GLint>(format),
            width,
            height,
            0,
            format,
            GL_UNSIGNED_BYTE,
            nullptr
        );
        for (int row = 0; row < height; ++row) {
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                row,
                width,
                1,
                format,
                GL_UNSIGNED_BYTE,
                source + static_cast<std::size_t>(row) * row_stride
            );
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    const GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        if (error_message != nullptr) {
            *error_message = "OpenGL failed while uploading the wallpaper texture";
        }
        return false;
    }

    frame_.kind = RenderFrameKind::RgbTexture;
    frame_.texture = texture;
    frame_.width = width;
    frame_.height = height;
    return true;
}

RuntimeSourcePtr make_static_image_source(std::string path) {
    return std::make_shared<StaticImageSource>(std::move(path), nullptr);
}

RuntimeSourcePtr make_static_image_source(
    std::shared_ptr<const std::string> bytes
) {
    return std::make_shared<StaticImageSource>(std::string{}, std::move(bytes));
}

} // namespace realmheart::wallpaper_native
