#include "effects/shell/ShellShaderRenderer.hpp"

#include "effects/core/EffectRegistry.hpp"
#include "effects/core/ShaderPlayback.hpp"
#include "effects/core/ShaderSource.hpp"

#include <epoxy/gl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::effects::shell {
namespace {

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

// gdk_texture_download() uses CAIRO_FORMAT_ARGB32. On little-endian hosts the
// bytes are B,G,R,A; OpenGL's GL_RGBA upload expects R,G,B,A.
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

struct ShellShaderRenderer::State {
    GtkWidget* gl_area = nullptr;
    GtkWidget* capture_parent = nullptr;
    GtkWidget* source_child = nullptr;

    bool active = false;
    bool frame_ready = false;
    bool opening = true;
    float timeline_progress = 0.0F;
    float corner_radius = 0.0F;
    ShaderPalette palette{};

    std::string fragment_asset;
    std::string fragment_source;
    std::string compiled_asset;

    std::vector<std::uint8_t> source_pixels;
    int source_width = 0;
    int source_height = 0;
    int source_logical_width = 0;
    int source_logical_height = 0;
    bool source_upload_pending = false;

    GLuint program = 0;
    GLuint vertex_array = 0;
    GLuint source_texture = 0;

    void release_texture() noexcept {
        if (source_texture == 0) return;
        if (gl_area != nullptr && gtk_widget_get_realized(gl_area)) {
            gtk_gl_area_make_current(GTK_GL_AREA(gl_area));
            if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area)) == nullptr) {
                glDeleteTextures(1, &source_texture);
            }
        }
        source_texture = 0;
    }

    void release_gl_resources() noexcept {
        if (gl_area == nullptr || !gtk_widget_get_realized(gl_area)) {
            source_texture = 0;
            vertex_array = 0;
            program = 0;
            compiled_asset.clear();
            return;
        }

        gtk_gl_area_make_current(GTK_GL_AREA(gl_area));
        if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area)) != nullptr) return;

        if (source_texture != 0) glDeleteTextures(1, &source_texture);
        if (vertex_array != 0) glDeleteVertexArrays(1, &vertex_array);
        if (program != 0) glDeleteProgram(program);
        source_texture = 0;
        vertex_array = 0;
        program = 0;
        compiled_asset.clear();
    }

    void restore_live_child() noexcept {
        if (source_child != nullptr) {
            gtk_widget_set_opacity(source_child, 1.0);
            gtk_widget_set_visible(source_child, TRUE);
            gtk_widget_queue_draw(source_child);
        }
        if (capture_parent != nullptr) gtk_widget_queue_draw(capture_parent);
    }

    void fail(std::string message) noexcept {
        std::cerr << "[Realmheart effects] Realmheart Void fallback: "
                  << message << '\n';
        active = false;
        frame_ready = false;
        source_upload_pending = false;
        release_texture();
        source_pixels.clear();
        source_pixels.shrink_to_fit();
        source_width = 0;
        source_height = 0;
        source_logical_width = 0;
        source_logical_height = 0;
        if (gl_area != nullptr) {
            gtk_widget_set_size_request(gl_area, -1, -1);
            gtk_widget_set_visible(gl_area, FALSE);
        }
        restore_live_child();
    }

    bool ensure_program(std::string* error) {
        if (program != 0 && compiled_asset == fragment_asset) return true;

        if (program != 0) {
            glDeleteProgram(program);
            program = 0;
        }
        if (vertex_array == 0) glGenVertexArrays(1, &vertex_array);

        program = link_program(fragment_source, error);
        if (program == 0) return false;
        compiled_asset = fragment_asset;
        return true;
    }

    bool upload_source(std::string* error) {
        if (!source_upload_pending) return source_texture != 0;
        if (source_pixels.empty() || source_width <= 0 || source_height <= 0) {
            set_error(error, "captured source texture is empty");
            return false;
        }

        if (source_texture == 0) glGenTextures(1, &source_texture);
        glBindTexture(GL_TEXTURE_2D, source_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
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
        glBindTexture(GL_TEXTURE_2D, 0);
        source_upload_pending = false;
        return true;
    }

    bool capture(std::string* error) {
        if (capture_parent == nullptr || source_child == nullptr) {
            set_error(error, "shader renderer is not attached to a source widget");
            return false;
        }

        const int width = gtk_widget_get_width(source_child);
        const int height = gtk_widget_get_height(source_child);
        if (width <= 0 || height <= 0) {
            set_error(error, "source widget has not been allocated yet");
            return false;
        }

        GtkNative* native = gtk_widget_get_native(capture_parent);
        GskRenderer* renderer = native != nullptr
            ? gtk_native_get_renderer(native)
            : nullptr;
        if (renderer == nullptr) {
            set_error(error, "source widget has no active GTK renderer");
            return false;
        }

        GtkSnapshot* snapshot = gtk_snapshot_new();
        gtk_widget_snapshot_child(capture_parent, source_child, snapshot);
        GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
        if (node == nullptr) {
            set_error(error, "source widget produced an empty snapshot");
            return false;
        }

        graphene_rect_t viewport;
        graphene_rect_init(
            &viewport,
            0.0F,
            0.0F,
            static_cast<float>(width),
            static_cast<float>(height)
        );
        GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &viewport);
        gsk_render_node_unref(node);
        if (texture == nullptr) {
            set_error(error, "GTK could not render the source snapshot to a texture");
            return false;
        }

        const int texture_width = gdk_texture_get_width(texture);
        const int texture_height = gdk_texture_get_height(texture);
        if (texture_width <= 0 || texture_height <= 0) {
            g_object_unref(texture);
            set_error(error, "captured source texture has invalid dimensions");
            return false;
        }

        const std::size_t stride = static_cast<std::size_t>(texture_width) * 4U;
        std::vector<std::uint8_t> pixels(
            stride * static_cast<std::size_t>(texture_height)
        );
        gdk_texture_download(texture, pixels.data(), stride);
        g_object_unref(texture);
        convert_argb32_to_rgba(pixels);

        std::uint8_t maximum_alpha = 0;
        for (std::size_t offset = 3; offset < pixels.size(); offset += 4) {
            maximum_alpha = std::max(maximum_alpha, pixels[offset]);
        }
        if (maximum_alpha == 0) {
            set_error(error, "captured source snapshot is fully transparent");
            return false;
        }

        std::cerr << "[Realmheart effects] Realmheart Void capture: "
                  << texture_width << 'x' << texture_height
                  << ", max alpha="
                  << static_cast<unsigned int>(maximum_alpha) << '\n';

        release_texture();
        source_pixels = std::move(pixels);
        source_width = texture_width;
        source_height = texture_height;
        source_logical_width = width;
        source_logical_height = height;
        source_upload_pending = true;

        // The parent may grow while a transition is running (for example when
        // launcher search results begin revealing). Keep the GL viewport fixed
        // to the exact logical allocation that was captured; otherwise GTK
        // stretches the frozen source texture to the parent's new height.
        gtk_widget_set_size_request(
            gl_area,
            source_logical_width,
            source_logical_height
        );
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
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        glBindVertexArray(vertex_array);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source_texture);

        const ShaderPlaybackFrame playback = sample_shader_playback(
            timeline_progress,
            opening
        );
        glUniform1f(
            glGetUniformLocation(program, "progress"),
            playback.progress
        );
        glUniform2f(
            glGetUniformLocation(program, "resolution"),
            static_cast<float>(source_width),
            static_cast<float>(source_height)
        );
        glUniform1i(glGetUniformLocation(program, "tex"), 0);
        glUniform1f(
            glGetUniformLocation(program, "radius"),
            corner_radius * static_cast<float>(scale)
        );
        glUniform1f(
            glGetUniformLocation(program, "reverse"),
            playback.reverse
        );
        glUniform3fv(
            glGetUniformLocation(program, "uGold"),
            1,
            palette.gold.data()
        );
        glUniform3fv(
            glGetUniformLocation(program, "uStarlight"),
            1,
            palette.starlight.data()
        );
        glUniform3fv(
            glGetUniformLocation(program, "uAstral"),
            1,
            palette.astral.data()
        );
        glUniform3fv(
            glGetUniformLocation(program, "uVoid"),
            1,
            palette.void_colour.data()
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

        if (!frame_ready) {
            frame_ready = true;
            if (source_child != nullptr) {
                gtk_widget_set_opacity(source_child, 0.0);
                gtk_widget_queue_draw(source_child);
            }
            if (capture_parent != nullptr) gtk_widget_queue_draw(capture_parent);
            std::cerr << "[Realmheart effects] Realmheart Void first frame ready: "
                      << width << 'x' << height << '\n';
        }
        return TRUE;
    }
};

ShellShaderRenderer::ShellShaderRenderer()
    : state_(new State) {
    state_->gl_area = gtk_gl_area_new();
    g_object_ref_sink(state_->gl_area);
    // A shader frame represents one captured allocation. It must never expand
    // merely because a sibling or the live source changes its natural size
    // during playback.
    gtk_widget_set_hexpand(state_->gl_area, FALSE);
    gtk_widget_set_vexpand(state_->gl_area, FALSE);
    gtk_widget_set_halign(state_->gl_area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(state_->gl_area, GTK_ALIGN_START);
    gtk_widget_set_can_target(state_->gl_area, FALSE);
    gtk_widget_set_focusable(state_->gl_area, FALSE);
    gtk_widget_set_visible(state_->gl_area, FALSE);

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

ShellShaderRenderer::~ShellShaderRenderer() {
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

GtkWidget* ShellShaderRenderer::widget() const noexcept {
    return state_ != nullptr ? state_->gl_area : nullptr;
}

bool ShellShaderRenderer::active() const noexcept {
    return state_ != nullptr && state_->active;
}

bool ShellShaderRenderer::frame_ready() const noexcept {
    return state_ != nullptr && state_->active && state_->frame_ready;
}

bool ShellShaderRenderer::begin(
    GtkWidget* capture_parent,
    GtkWidget* source_child,
    EffectId effect,
    bool opening,
    double corner_radius,
    const ShaderPalette& palette,
    std::string* error
) {
    if (state_ == nullptr || capture_parent == nullptr || source_child == nullptr) {
        set_error(error, "shader renderer is not attached to a source widget");
        return false;
    }

    const EffectSpec* spec = find_effect(effect);
    if (spec == nullptr || spec->backend != EffectBackend::Shader ||
        spec->fragment_shader_asset.empty()) {
        set_error(error, "requested effect is not a registered shader");
        return false;
    }

    std::string load_error;
    const auto source = load_shader_source(
        spec->fragment_shader_asset,
        &load_error
    );
    if (!source) {
        set_error(error, std::move(load_error));
        return false;
    }

    std::string missing_symbol;
    if (!validate_shell_shader_contract(source->text, &missing_symbol)) {
        set_error(error, "shader contract is missing: " + missing_symbol);
        return false;
    }

    finish();
    state_->capture_parent = capture_parent;
    state_->source_child = source_child;
    state_->fragment_asset = std::string{spec->fragment_shader_asset};
    state_->fragment_source = source->text;
    state_->opening = opening;
    state_->timeline_progress = opening ? 0.0F : 1.0F;
    state_->corner_radius = static_cast<float>(std::max(corner_radius, 0.0));
    state_->palette = palette;

    std::string capture_error;
    if (!state_->capture(&capture_error)) {
        set_error(error, std::move(capture_error));
        state_->restore_live_child();
        return false;
    }

    state_->active = true;
    state_->frame_ready = false;
    gtk_widget_set_visible(state_->gl_area, TRUE);
    gtk_widget_set_opacity(state_->gl_area, 1.0);
    gtk_widget_set_visible(source_child, TRUE);
    // Opening starts from a deliberately transparent shader frame. Once the
    // source has been captured, hide the live child immediately so it cannot
    // flash above that first frame. Closing keeps the live child visible until
    // GL has produced its replacement frame.
    gtk_widget_set_opacity(source_child, opening ? 0.0 : 1.0);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
    gtk_widget_queue_draw(capture_parent);

    if (error != nullptr) error->clear();
    return true;
}

void ShellShaderRenderer::update(
    double timeline_progress,
    bool opening
) noexcept {
    if (state_ == nullptr || !state_->active) return;
    state_->timeline_progress = static_cast<float>(
        std::clamp(timeline_progress, 0.0, 1.0)
    );
    state_->opening = opening;
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
    if (state_->capture_parent != nullptr) {
        gtk_widget_queue_draw(state_->capture_parent);
    }
}

void ShellShaderRenderer::finish() noexcept {
    if (state_ == nullptr) return;
    state_->active = false;
    state_->frame_ready = false;
    state_->source_upload_pending = false;
    state_->release_texture();
    state_->source_pixels.clear();
    state_->source_pixels.shrink_to_fit();
    state_->source_width = 0;
    state_->source_height = 0;
    state_->source_logical_width = 0;
    state_->source_logical_height = 0;
    if (state_->gl_area != nullptr) {
        gtk_widget_set_opacity(state_->gl_area, 0.0);
        gtk_widget_set_size_request(state_->gl_area, -1, -1);
        gtk_widget_set_visible(state_->gl_area, FALSE);
    }
    state_->restore_live_child();
}

} // namespace realmheart::effects::shell
