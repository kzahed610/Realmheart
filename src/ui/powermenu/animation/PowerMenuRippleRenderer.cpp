#include "ui/powermenu/animation/PowerMenuRippleRenderer.hpp"

#include "effects/core/ShaderSource.hpp"

#include <epoxy/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::ui::powermenu::animation {
namespace {

constexpr std::string_view kShaderAsset =
    "power-menu/ripple-reveal/ripple-reveal.frag";

constexpr std::string_view kVertexShader = R"GLSL(#version 300 es
precision highp float;

out vec2 v_texcoord;

void main() {
    vec2 corner = vec2(
        float((gl_VertexID << 1) & 2),
        float(gl_VertexID & 2)
    );
    v_texcoord = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

float sanitize_unit(double value, float fallback) noexcept {
    if (!std::isfinite(value)) return fallback;
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

GLuint compile_shader(
    GLenum type,
    std::string_view source,
    std::string* error
) {
    const GLuint shader = glCreateShader(type);
    const char* source_pointer = source.data();
    const GLint source_length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &source_pointer, &source_length);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    set_error(error, std::move(log));
    return 0;
}

GLuint link_program(
    std::string_view fragment_source,
    std::string* error
) {
    std::string vertex_error;
    const GLuint vertex = compile_shader(
        GL_VERTEX_SHADER,
        kVertexShader,
        &vertex_error
    );
    if (vertex == 0) {
        set_error(error, "vertex shader compilation failed: " + vertex_error);
        return 0;
    }

    std::string fragment_error;
    const GLuint fragment = compile_shader(
        GL_FRAGMENT_SHADER,
        fragment_source,
        &fragment_error
    );
    if (fragment == 0) {
        glDeleteShader(vertex);
        set_error(error, "fragment shader compilation failed: " + fragment_error);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) return program;

    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    glDeleteProgram(program);
    set_error(error, "shader link failed: " + log);
    return 0;
}

// gdk_texture_download() exposes CAIRO_FORMAT_ARGB32 bytes. On little-endian
// systems those bytes are B,G,R,A, while the GLES upload expects R,G,B,A.
void convert_argb32_to_rgba(std::vector<std::uint8_t>& pixels) {
    for (std::size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
        std::swap(pixels[offset], pixels[offset + 2]);
#else
        const std::uint8_t alpha = pixels[offset];
        const std::uint8_t red = pixels[offset + 1];
        const std::uint8_t green = pixels[offset + 2];
        const std::uint8_t blue = pixels[offset + 3];
        pixels[offset] = red;
        pixels[offset + 1] = green;
        pixels[offset + 2] = blue;
        pixels[offset + 3] = alpha;
#endif
    }
}

} // namespace

struct PowerMenuRippleRenderer::State {
    GtkWidget* gl_area = nullptr;

    bool active = false;
    bool frame_ready = false;
    bool opening = true;
    float progress = 0.0F;
    float origin_x = 0.012F;
    float origin_y = 0.94F;

    std::string fragment_source;
    std::vector<std::uint8_t> source_pixels;
    int source_width = 0;
    int source_height = 0;
    bool source_upload_pending = false;

    GLuint program = 0;
    GLuint vertex_array = 0;
    GLuint source_texture = 0;
    int texture_width = 0;
    int texture_height = 0;

    static constexpr std::array<float, 3> kGold{
        1.000F, 0.790F, 0.310F
    };
    static constexpr std::array<float, 3> kStarlight{
        0.650F, 0.890F, 1.000F
    };
    static constexpr std::array<float, 3> kAstral{
        0.510F, 0.300F, 1.000F
    };

    void release_texture() noexcept {
        if (source_texture == 0) return;
        if (gl_area != nullptr && gtk_widget_get_realized(gl_area)) {
            gtk_gl_area_make_current(GTK_GL_AREA(gl_area));
            if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area)) == nullptr) {
                glDeleteTextures(1, &source_texture);
            }
        }
        source_texture = 0;
        texture_width = 0;
        texture_height = 0;
    }

    void release_gl_resources() noexcept {
        if (gl_area == nullptr || !gtk_widget_get_realized(gl_area)) {
            source_texture = 0;
            texture_width = 0;
            texture_height = 0;
            vertex_array = 0;
            program = 0;
            return;
        }

        gtk_gl_area_make_current(GTK_GL_AREA(gl_area));
        if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area)) != nullptr) return;

        if (source_texture != 0) glDeleteTextures(1, &source_texture);
        if (vertex_array != 0) glDeleteVertexArrays(1, &vertex_array);
        if (program != 0) glDeleteProgram(program);
        source_texture = 0;
        texture_width = 0;
        texture_height = 0;
        vertex_array = 0;
        program = 0;
    }

    void fail(std::string message) noexcept {
        std::cerr << "[PowerMenuRipple] GL fallback: " << message << '\n';
        active = false;
        frame_ready = false;
        source_upload_pending = false;
        release_texture();
        source_pixels.clear();
        source_pixels.shrink_to_fit();
        source_width = 0;
        source_height = 0;
        if (gl_area != nullptr) {
            gtk_widget_set_opacity(gl_area, 0.0);
            gtk_widget_set_visible(gl_area, FALSE);
        }
    }

    bool capture(GdkPaintable* source, std::string* error) {
        if (source == nullptr || !GDK_IS_PAINTABLE(source)) {
            set_error(error, "source paintable is unavailable");
            return false;
        }

        GdkPaintable* image = gdk_paintable_get_current_image(source);
        if (image == nullptr || !GDK_IS_TEXTURE(image)) {
            g_clear_object(&image);
            set_error(error, "source paintable has no texture-backed frame yet");
            return false;
        }

        GdkTexture* texture = GDK_TEXTURE(image);
        const int width = gdk_texture_get_width(texture);
        const int height = gdk_texture_get_height(texture);
        constexpr std::size_t kBytesPerPixel = 4U;
        if (width <= 0 || height <= 0 ||
            static_cast<std::size_t>(width) >
                (std::numeric_limits<std::size_t>::max() / kBytesPerPixel)) {
            g_object_unref(image);
            set_error(error, "source texture has invalid dimensions");
            return false;
        }

        const std::size_t stride = static_cast<std::size_t>(width) * kBytesPerPixel;
        if (static_cast<std::size_t>(height) >
            (std::numeric_limits<std::size_t>::max() / stride)) {
            g_object_unref(image);
            set_error(error, "source texture is too large to capture safely");
            return false;
        }

        const std::size_t byte_count =
            stride * static_cast<std::size_t>(height);

        // Reuse one full-frame CPU staging allocation after the first capture.
        // Opening and closing both upload a 1080p frame; repeatedly allocating
        // and freeing that buffer causes allocator arenas to grow across cycles.
        source_pixels.resize(byte_count);
        gdk_texture_download(texture, source_pixels.data(), stride);
        g_object_unref(image);
        convert_argb32_to_rgba(source_pixels);

        std::uint8_t maximum_alpha = 0;
        for (std::size_t offset = 3; offset < source_pixels.size(); offset += 4) {
            maximum_alpha = std::max(maximum_alpha, source_pixels[offset]);
        }
        if (maximum_alpha == 0) {
            source_pixels.clear();
            set_error(error, "captured source frame is fully transparent");
            return false;
        }

        source_width = width;
        source_height = height;
        source_upload_pending = true;
        return true;
    }

    bool ensure_program(std::string* error) {
        if (program != 0) return true;
        if (fragment_source.empty()) {
            set_error(error, "ripple shader source is empty");
            return false;
        }

        program = link_program(fragment_source, error);
        if (program == 0) return false;
        glGenVertexArrays(1, &vertex_array);
        return true;
    }

    bool upload_source(std::string* error) {
        if (!source_upload_pending) return source_texture != 0;
        if (source_pixels.empty() || source_width <= 0 || source_height <= 0) {
            set_error(error, "captured source texture is empty");
            return false;
        }

        if (source_texture == 0) {
            glGenTextures(1, &source_texture);
            glBindTexture(GL_TEXTURE_2D, source_texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, source_texture);
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (texture_width == source_width && texture_height == source_height) {
            // Same video geometry on every normal invocation: overwrite the
            // existing storage instead of forcing Mesa to allocate another BO.
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                source_width,
                source_height,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                source_pixels.data()
            );
        } else {
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA8,
                source_width,
                source_height,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                source_pixels.data()
            );
            texture_width = source_width;
            texture_height = source_height;
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        source_upload_pending = false;

        // Keep the allocation capacity but mark the staging buffer empty. It is
        // reused by the next capture and does not schedule any background work.
        source_pixels.clear();
        return true;
    }

    gboolean render(GtkGLArea* area) noexcept {
        if (!active) return TRUE;

        if (const GError* gl_error = gtk_gl_area_get_error(area);
            gl_error != nullptr) {
            fail(gl_error->message != nullptr
                ? gl_error->message
                : "OpenGL context error");
            return TRUE;
        }

        std::string error;
        if (!ensure_program(&error) || !upload_source(&error)) {
            fail(std::move(error));
            return TRUE;
        }

        const int scale = std::max(
            gtk_widget_get_scale_factor(GTK_WIDGET(area)),
            1
        );
        const int width = std::max(
            gtk_widget_get_width(GTK_WIDGET(area)) * scale,
            1
        );
        const int height = std::max(
            gtk_widget_get_height(GTK_WIDGET(area)) * scale,
            1
        );

        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        glBindVertexArray(vertex_array);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source_texture);

        glUniform1f(glGetUniformLocation(program, "progress"), progress);
        glUniform2f(
            glGetUniformLocation(program, "resolution"),
            static_cast<float>(width),
            static_cast<float>(height)
        );
        glUniform1i(glGetUniformLocation(program, "tex"), 0);
        glUniform2f(
            glGetUniformLocation(program, "origin"),
            origin_x,
            origin_y
        );
        glUniform1f(
            glGetUniformLocation(program, "opening"),
            opening ? 1.0F : 0.0F
        );
        glUniform3fv(
            glGetUniformLocation(program, "uGold"),
            1,
            kGold.data()
        );
        glUniform3fv(
            glGetUniformLocation(program, "uStarlight"),
            1,
            kStarlight.data()
        );
        glUniform3fv(
            glGetUniformLocation(program, "uAstral"),
            1,
            kAstral.data()
        );

        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindVertexArray(0);
        glUseProgram(0);

        const GLenum draw_error = glGetError();
        if (draw_error != GL_NO_ERROR) {
            fail("OpenGL draw failed with error " +
                 std::to_string(static_cast<unsigned int>(draw_error)));
            return TRUE;
        }

        frame_ready = true;
        return TRUE;
    }
};

PowerMenuRippleRenderer::PowerMenuRippleRenderer()
    : state_(new State) {
    state_->gl_area = gtk_gl_area_new();
    g_object_ref_sink(state_->gl_area);
    gtk_widget_set_hexpand(state_->gl_area, TRUE);
    gtk_widget_set_vexpand(state_->gl_area, TRUE);
    gtk_widget_set_halign(state_->gl_area, GTK_ALIGN_FILL);
    gtk_widget_set_valign(state_->gl_area, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(state_->gl_area, FALSE);
    gtk_widget_set_focusable(state_->gl_area, FALSE);
    gtk_widget_add_css_class(state_->gl_area, "realmheart-power-menu-ripple");
    gtk_widget_remove_css_class(state_->gl_area, "background");
    gtk_widget_set_visible(state_->gl_area, FALSE);
    gtk_widget_set_opacity(state_->gl_area, 0.0);

    gtk_gl_area_set_allowed_apis(
        GTK_GL_AREA(state_->gl_area),
        GDK_GL_API_GLES
    );
    gtk_gl_area_set_required_version(GTK_GL_AREA(state_->gl_area), 3, 0);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(state_->gl_area), FALSE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(state_->gl_area), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(state_->gl_area), FALSE);

    g_signal_connect(
        state_->gl_area,
        "render",
        G_CALLBACK(+[](
            GtkGLArea* area,
            GdkGLContext*,
            gpointer data
        ) -> gboolean {
            return static_cast<State*>(data)->render(area);
        }),
        state_
    );
    g_signal_connect(
        state_->gl_area,
        "unrealize",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            static_cast<State*>(data)->release_gl_resources();
        }),
        state_
    );
}

PowerMenuRippleRenderer::~PowerMenuRippleRenderer() {
    if (state_ == nullptr) return;
    finish();
    state_->release_gl_resources();
    if (state_->gl_area != nullptr) {
        g_signal_handlers_disconnect_by_data(state_->gl_area, state_);
        g_object_unref(state_->gl_area);
        state_->gl_area = nullptr;
    }
    delete state_;
    state_ = nullptr;
}

GtkWidget* PowerMenuRippleRenderer::widget() const noexcept {
    return state_ != nullptr ? state_->gl_area : nullptr;
}

bool PowerMenuRippleRenderer::active() const noexcept {
    return state_ != nullptr && state_->active;
}

bool PowerMenuRippleRenderer::frame_ready() const noexcept {
    return state_ != nullptr && state_->active && state_->frame_ready;
}

bool PowerMenuRippleRenderer::begin(
    GdkPaintable* source,
    double normalized_origin_x,
    double normalized_origin_y,
    bool opening,
    std::string* error
) {
    if (state_ == nullptr) {
        set_error(error, "ripple renderer is unavailable");
        return false;
    }

    if (state_->fragment_source.empty()) {
        std::string load_error;
        const auto shader = realmheart::effects::load_shader_source(
            kShaderAsset,
            &load_error
        );
        if (!shader) {
            set_error(error, std::move(load_error));
            return false;
        }

        std::string missing_symbol;
        if (!realmheart::effects::validate_power_menu_ripple_shader_contract(
                shader->text,
                &missing_symbol)) {
            set_error(error, "ripple shader contract is missing: " + missing_symbol);
            return false;
        }
        state_->fragment_source = shader->text;
    }

    finish();
    state_->origin_x = sanitize_unit(normalized_origin_x, 0.012F);
    state_->origin_y = sanitize_unit(normalized_origin_y, 0.94F);
    state_->opening = opening;
    state_->progress = opening ? 0.0F : 1.0F;

    std::string capture_error;
    if (!state_->capture(source, &capture_error)) {
        set_error(error, std::move(capture_error));
        return false;
    }

    state_->active = true;
    state_->frame_ready = false;
    gtk_widget_set_visible(state_->gl_area, TRUE);
    gtk_widget_set_opacity(state_->gl_area, 1.0);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));

    if (error != nullptr) error->clear();
    std::cerr << "[PowerMenuRipple] captured transition frame: "
              << state_->source_width << 'x' << state_->source_height
              << " origin=" << state_->origin_x << ',' << state_->origin_y
              << '\n';
    return true;
}

void PowerMenuRippleRenderer::update(
    double progress,
    bool opening
) noexcept {
    if (state_ == nullptr || !state_->active) return;
    state_->progress = sanitize_unit(progress, state_->progress);
    state_->opening = opening;
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
}

void PowerMenuRippleRenderer::set_opacity(double opacity) noexcept {
    if (state_ == nullptr || state_->gl_area == nullptr) return;
    const double safe_opacity = std::isfinite(opacity)
        ? std::clamp(opacity, 0.0, 1.0)
        : 1.0;
    gtk_widget_set_opacity(state_->gl_area, safe_opacity);
}

void PowerMenuRippleRenderer::finish() noexcept {
    if (state_ == nullptr) return;
    state_->active = false;
    state_->frame_ready = false;
    state_->source_upload_pending = false;

    // The GL area is non-auto-rendering and hidden below, so retaining one
    // texture object and one staging-buffer capacity costs no idle GPU time.
    // Reusing them avoids repeated fullscreen allocation/deallocation churn.
    state_->source_pixels.clear();
    state_->source_width = 0;
    state_->source_height = 0;
    if (state_->gl_area != nullptr) {
        gtk_widget_set_opacity(state_->gl_area, 0.0);
        gtk_widget_set_visible(state_->gl_area, FALSE);
    }
}

} // namespace realmheart::ui::powermenu::animation
