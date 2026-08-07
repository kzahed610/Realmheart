#include "ui/workspace/animation/WorkspaceOverviewMorphRenderer.hpp"

#include "effects/core/ShaderSource.hpp"
#include "ui/workspace/animation/WorkspaceMorphRendererState.hpp"

#include <epoxy/gl.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::ui::workspace::animation {
namespace {

constexpr std::string_view kFragmentAsset =
    "workspace/elemental-morph/elemental-morph.frag";

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

void discard_gl_errors() noexcept {
    // Error state is sticky. A harmless command issued while GtkGLArea still
    // had a 0x0 allocation must not poison the first real draw and masquerade
    // as a shader failure. Keep the loop bounded in case a broken driver keeps
    // reporting the same condition.
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (glGetError() == GL_NO_ERROR) return;
    }
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
        set_error(
            error,
            "fragment shader compilation failed: " + fragment_error
        );
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

struct WorkspaceOverviewMorphRenderer::State {
    GtkWidget* gl_area = nullptr;
    GtkWidget* capture_parent = nullptr;
    GtkWidget* source_child = nullptr;

    WorkspaceMorphRendererState lifecycle{};
    WorkspaceMorphShaderGeometry geometry{};
    WorkspaceMorphShaderFrame frame{};
    WorkspaceMorphShaderPalette palette{};

    std::string fragment_source;
    std::vector<std::uint8_t> source_pixels;
    int source_width = 0;
    int source_height = 0;
    int source_logical_width = 0;
    int source_logical_height = 0;
    std::size_t captured_source_bytes = 0;
    bool source_upload_pending = false;
    bool failed = false;
    bool allocation_wait_logged = false;
    bool framebuffer_wait_logged = false;

    struct UniformLocations {
        GLint progress = -1;
        GLint opening = -1;
        GLint resolution = -1;
        GLint tex = -1;
        GLint origin = -1;
        GLint element_style = -1;
        GLint source_y = -1;
        GLint reveal_left_x = -1;
        GLint front_x = -1;
        GLint front_top = -1;
        GLint front_bottom = -1;
        GLint gold = -1;
        GLint starlight = -1;
        GLint astral = -1;
        GLint void_colour = -1;

        [[nodiscard]] bool valid() const noexcept {
            return progress >= 0 && opening >= 0 && resolution >= 0 &&
                tex >= 0 && origin >= 0 && element_style >= 0 &&
                source_y >= 0 &&
                reveal_left_x >= 0 && front_x >= 0 && front_top >= 0 &&
                front_bottom >= 0 && gold >= 0 && starlight >= 0 &&
                astral >= 0 && void_colour >= 0;
        }
    };

    GLuint program = 0;
    GLuint vertex_array = 0;
    GLuint source_texture = 0;
    UniformLocations uniforms{};

    void clear_framebuffer() noexcept {
        if (gl_area == nullptr || !gtk_widget_get_realized(gl_area)) return;
        const int logical_width = gtk_widget_get_width(gl_area);
        const int logical_height = gtk_widget_get_height(gl_area);
        if (logical_width <= 0 || logical_height <= 0) return;

        auto* area = GTK_GL_AREA(gl_area);
        gtk_gl_area_make_current(area);
        if (gtk_gl_area_get_error(area) != nullptr) return;
        discard_gl_errors();
        gtk_gl_area_attach_buffers(area);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            discard_gl_errors();
            return;
        }

        const int scale = std::max(gtk_widget_get_scale_factor(gl_area), 1);
        glViewport(0, 0, logical_width * scale, logical_height * scale);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();
        discard_gl_errors();
    }

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
            uniforms = {};
            return;
        }

        gtk_gl_area_make_current(GTK_GL_AREA(gl_area));
        if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area)) != nullptr) {
            source_texture = 0;
            vertex_array = 0;
            program = 0;
            uniforms = {};
            return;
        }

        if (source_texture != 0) glDeleteTextures(1, &source_texture);
        if (vertex_array != 0) glDeleteVertexArrays(1, &vertex_array);
        if (program != 0) glDeleteProgram(program);
        source_texture = 0;
        vertex_array = 0;
        program = 0;
        uniforms = {};
    }

    void fail(std::string message) noexcept {
        std::cerr << "[Realmheart workspace morph] Shader fallback: "
                  << message << '\n';
        lifecycle.finish();
        failed = true;
        allocation_wait_logged = false;
        framebuffer_wait_logged = false;
        source_upload_pending = false;
        release_texture();
        source_pixels.clear();
        source_pixels.shrink_to_fit();
        source_width = 0;
        source_height = 0;
        source_logical_width = 0;
        source_logical_height = 0;
        captured_source_bytes = 0;
        if (gl_area != nullptr) {
            gtk_widget_set_opacity(gl_area, 0.0);
            gtk_widget_set_size_request(gl_area, -1, -1);
            gtk_widget_set_visible(gl_area, FALSE);
        }
        if (capture_parent != nullptr) gtk_widget_queue_draw(capture_parent);
    }

    bool ensure_program(std::string* error) {
        if (program != 0) return true;
        if (vertex_array == 0) glGenVertexArrays(1, &vertex_array);
        program = link_program(fragment_source, error);
        if (program == 0) return false;

        uniforms.progress = glGetUniformLocation(program, "progress");
        uniforms.opening = glGetUniformLocation(program, "opening");
        uniforms.resolution = glGetUniformLocation(program, "resolution");
        uniforms.tex = glGetUniformLocation(program, "tex");
        uniforms.origin = glGetUniformLocation(program, "origin");
        uniforms.element_style = glGetUniformLocation(program, "elementStyle");
        uniforms.source_y = glGetUniformLocation(program, "sourceY");
        uniforms.reveal_left_x = glGetUniformLocation(program, "revealLeftX");
        uniforms.front_x = glGetUniformLocation(program, "frontX");
        uniforms.front_top = glGetUniformLocation(program, "frontTop");
        uniforms.front_bottom = glGetUniformLocation(program, "frontBottom");
        uniforms.gold = glGetUniformLocation(program, "uGold");
        uniforms.starlight = glGetUniformLocation(program, "uStarlight");
        uniforms.astral = glGetUniformLocation(program, "uAstral");
        uniforms.void_colour = glGetUniformLocation(program, "uVoid");
        if (uniforms.valid()) return true;

        glDeleteProgram(program);
        program = 0;
        uniforms = {};
        set_error(error, "compiled shader is missing a required uniform");
        return false;
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
        source_pixels.clear();
        source_pixels.shrink_to_fit();
        return true;
    }

    bool capture(std::string* error) {
        if (capture_parent == nullptr || source_child == nullptr) {
            set_error(error, "morph renderer is not attached to a source widget");
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

        const graphene_rect_t viewport = GRAPHENE_RECT_INIT(
            0.0F,
            0.0F,
            static_cast<float>(width),
            static_cast<float>(height)
        );
        GdkTexture* texture = gsk_renderer_render_texture(
            renderer,
            node,
            &viewport
        );
        gsk_render_node_unref(node);
        if (texture == nullptr) {
            set_error(
                error,
                "GTK could not render the overview snapshot to a texture"
            );
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
            set_error(error, "captured overview snapshot is fully transparent");
            return false;
        }

        release_texture();
        source_pixels = std::move(pixels);
        captured_source_bytes = source_pixels.size();
        source_width = texture_width;
        source_height = texture_height;
        source_logical_width = width;
        source_logical_height = height;
        source_upload_pending = true;
        gtk_widget_set_size_request(
            gl_area,
            source_logical_width,
            source_logical_height
        );

        std::cerr << "[Realmheart workspace morph] Captured overview: "
                  << texture_width << 'x' << texture_height << '\n';
        return true;
    }

    gboolean render(GtkGLArea* area) noexcept {
        if (!lifecycle.active()) return TRUE;

        if (const GError* gl_error = gtk_gl_area_get_error(area);
            gl_error != nullptr) {
            fail(gl_error->message != nullptr
                ? gl_error->message
                : "OpenGL context error");
            return TRUE;
        }

        GtkWidget* widget = GTK_WIDGET(area);
        const int logical_width = gtk_widget_get_width(widget);
        const int logical_height = gtk_widget_get_height(widget);
        if (logical_width <= 0 || logical_height <= 0) {
            if (!allocation_wait_logged) {
                std::cerr
                    << "[Realmheart workspace morph] GL host waiting for allocation\n";
                allocation_wait_logged = true;
            }
            return TRUE;
        }
        allocation_wait_logged = false;

        // GtkGLArea normally binds its target before ::render, but explicitly
        // attaching here also handles the first frame after a visibility/size
        // transition. Never draw into the transient 0x0 framebuffer.
        discard_gl_errors();
        gtk_gl_area_attach_buffers(area);
        const GLenum framebuffer_status =
            glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
            if (!framebuffer_wait_logged) {
                std::cerr
                    << "[Realmheart workspace morph] GL framebuffer waiting: "
                    << static_cast<unsigned int>(framebuffer_status) << '\n';
                framebuffer_wait_logged = true;
            }
            discard_gl_errors();
            return TRUE;
        }
        framebuffer_wait_logged = false;
        discard_gl_errors();

        std::string error;
        if (!ensure_program(&error) || !upload_source(&error)) {
            fail(std::move(error));
            return TRUE;
        }

        const int scale = std::max(gtk_widget_get_scale_factor(widget), 1);
        const int width = logical_width * scale;
        const int height = logical_height * scale;

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

        glUniform1f(
            uniforms.progress,
            static_cast<float>(lifecycle.progress())
        );
        glUniform1f(uniforms.opening, lifecycle.opening() ? 1.0F : 0.0F);
        glUniform2f(
            uniforms.resolution,
            static_cast<float>(source_width),
            static_cast<float>(source_height)
        );
        glUniform1i(uniforms.tex, 0);
        glUniform2fv(uniforms.origin, 1, geometry.origin.data());
        glUniform4fv(
            uniforms.element_style,
            1,
            geometry.element_style.data()
        );
        glUniform4fv(uniforms.source_y, 1, geometry.source_y.data());
        glUniform4fv(
            uniforms.reveal_left_x,
            1,
            frame.reveal_left_x.data()
        );
        glUniform4fv(uniforms.front_x, 1, frame.front_x.data());
        glUniform4fv(uniforms.front_top, 1, frame.front_top.data());
        glUniform4fv(
            uniforms.front_bottom,
            1,
            frame.front_bottom.data()
        );
        glUniform3fv(uniforms.gold, 1, palette.gold.data());
        glUniform3fv(uniforms.starlight, 1, palette.starlight.data());
        glUniform3fv(uniforms.astral, 1, palette.astral.data());
        glUniform3fv(
            uniforms.void_colour,
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

        if (!lifecycle.frame_ready()) {
            lifecycle.mark_frame_ready();
            gtk_widget_set_opacity(
                gl_area,
                workspace_morph_overlay_opacity(
                    lifecycle.progress(),
                    lifecycle.frame_ready()
                )
            );
            if (capture_parent != nullptr) {
                gtk_widget_queue_draw(capture_parent);
            }
            std::cerr << "[Realmheart workspace morph] First shader frame ready: "
                      << width << 'x' << height << '\n';
        }
        return TRUE;
    }

};

WorkspaceOverviewMorphRenderer::WorkspaceOverviewMorphRenderer()
    : state_(new State) {
    state_->gl_area = gtk_gl_area_new();
    g_object_ref_sink(state_->gl_area);
    gtk_widget_set_hexpand(state_->gl_area, TRUE);
    gtk_widget_set_vexpand(state_->gl_area, TRUE);
    gtk_widget_set_halign(state_->gl_area, GTK_ALIGN_FILL);
    gtk_widget_set_valign(state_->gl_area, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(state_->gl_area, FALSE);
    gtk_widget_set_focusable(state_->gl_area, FALSE);
    gtk_widget_add_css_class(
        state_->gl_area,
        "realmheart-workspace-morph-gl-area"
    );
    gtk_widget_set_opacity(state_->gl_area, 0.0);
    gtk_widget_set_visible(state_->gl_area, FALSE);

    gtk_gl_area_set_allowed_apis(
        GTK_GL_AREA(state_->gl_area),
        GDK_GL_API_GLES
    );
    gtk_gl_area_set_required_version(GTK_GL_AREA(state_->gl_area), 3, 0);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(state_->gl_area), TRUE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(state_->gl_area), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(state_->gl_area), FALSE);

    g_signal_connect(
        state_->gl_area,
        "realize",
        G_CALLBACK(+[](GtkWidget* widget, gpointer) {
            auto* area = GTK_GL_AREA(widget);
            gtk_gl_area_make_current(area);
            if (const GError* gl_error = gtk_gl_area_get_error(area);
                gl_error != nullptr) {
                std::cerr
                    << "[Realmheart workspace morph] GL host realization error: "
                    << (gl_error->message != nullptr
                        ? gl_error->message
                        : "OpenGL context realization failed")
                    << '\n';
                return;
            }
            std::cerr << "[Realmheart workspace morph] GL host realized: "
                      << gtk_widget_get_width(widget) << 'x'
                      << gtk_widget_get_height(widget) << '\n';
        }),
        state_
    );
    g_signal_connect(
        state_->gl_area,
        "map",
        G_CALLBACK(+[](GtkWidget* widget, gpointer data) {
            auto* state = static_cast<State*>(data);
            if (!state->lifecycle.active()) return;
            gtk_widget_set_opacity(
                widget,
                workspace_morph_overlay_opacity(
                    state->lifecycle.progress(),
                    state->lifecycle.frame_ready()
                )
            );
            const int width = gtk_widget_get_width(widget);
            const int height = gtk_widget_get_height(widget);
            if (width > 0 && height > 0) {
                gtk_widget_queue_draw(widget);
                gtk_gl_area_queue_render(GTK_GL_AREA(widget));
            } else {
                gtk_widget_queue_resize(widget);
            }
            std::cerr << "[Realmheart workspace morph] GL host mapped: "
                      << width << 'x' << height << '\n';
        }),
        state_
    );
    g_signal_connect(
        state_->gl_area,
        "resize",
        G_CALLBACK(+[](
            GtkGLArea* area,
            int width,
            int height,
            gpointer data
        ) {
            auto* state = static_cast<State*>(data);
            if (!state->lifecycle.active() || width <= 0 || height <= 0) {
                return;
            }
            std::cerr << "[Realmheart workspace morph] GL host allocated: "
                      << width << 'x' << height << '\n';
            gtk_widget_queue_draw(GTK_WIDGET(area));
            gtk_gl_area_queue_render(area);
        }),
        state_
    );
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

WorkspaceOverviewMorphRenderer::~WorkspaceOverviewMorphRenderer() {
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

GtkWidget* WorkspaceOverviewMorphRenderer::widget() const noexcept {
    return state_ != nullptr ? state_->gl_area : nullptr;
}

bool WorkspaceOverviewMorphRenderer::active() const noexcept {
    return state_ != nullptr && state_->lifecycle.active();
}

bool WorkspaceOverviewMorphRenderer::frame_ready() const noexcept {
    return state_ != nullptr && state_->lifecycle.frame_ready();
}

bool WorkspaceOverviewMorphRenderer::failed() const noexcept {
    return state_ != nullptr && state_->failed;
}

std::size_t WorkspaceOverviewMorphRenderer::captured_source_bytes() const noexcept {
    return state_ != nullptr ? state_->captured_source_bytes : 0U;
}

std::size_t WorkspaceOverviewMorphRenderer::transient_source_bytes() const noexcept {
    if (state_ == nullptr) return 0U;
    const std::size_t texture_bytes = state_->source_texture != 0
        ? state_->captured_source_bytes
        : 0U;
    return state_->source_pixels.size() + texture_bytes;
}

bool WorkspaceOverviewMorphRenderer::begin(
    GtkWidget* capture_parent,
    GtkWidget* source_child,
    bool opening,
    const WorkspaceMorphShaderGeometry& geometry,
    const WorkspaceMorphShaderPalette& palette,
    std::string* error
) {
    if (state_ == nullptr || capture_parent == nullptr || source_child == nullptr) {
        set_error(error, "morph renderer is not attached to a source widget");
        return false;
    }

    std::string load_error;
    const auto source = effects::load_shader_source(kFragmentAsset, &load_error);
    if (!source) {
        set_error(error, std::move(load_error));
        return false;
    }

    std::string missing_symbol;
    if (!effects::validate_workspace_morph_shader_contract(
            source->text,
            &missing_symbol
        )) {
        set_error(error, "shader contract is missing: " + missing_symbol);
        return false;
    }

    finish();
    state_->failed = false;
    state_->capture_parent = capture_parent;
    state_->source_child = source_child;
    state_->fragment_source = source->text;
    state_->geometry = geometry;
    state_->frame = {};
    state_->palette = palette;

    std::string capture_error;
    if (!state_->capture(&capture_error)) {
        set_error(error, std::move(capture_error));
        return false;
    }

    state_->lifecycle.begin(opening);
    gtk_widget_set_visible(state_->gl_area, TRUE);
    gtk_widget_set_opacity(
        state_->gl_area,
        workspace_morph_overlay_opacity(
            state_->lifecycle.progress(),
            state_->lifecycle.frame_ready()
        )
    );
    state_->allocation_wait_logged = false;
    state_->framebuffer_wait_logged = false;
    gtk_widget_queue_resize(state_->gl_area);
    // Do not force realization while the overlay still has a 0x0 allocation.
    // GTK will realize/map it as part of the presented widget tree, then the
    // resize callback queues the first draw against a complete framebuffer.
    gtk_widget_queue_draw(state_->gl_area);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
    gtk_widget_queue_draw(capture_parent);

    if (error != nullptr) error->clear();
    return true;
}

void WorkspaceOverviewMorphRenderer::update(
    double progress,
    bool opening,
    const WorkspaceMorphShaderFrame& frame
) noexcept {
    if (state_ == nullptr || !state_->lifecycle.active()) return;
    state_->lifecycle.update(progress, opening);
    state_->frame = frame;
    gtk_widget_set_opacity(
        state_->gl_area,
        workspace_morph_overlay_opacity(
            state_->lifecycle.progress(),
            state_->lifecycle.frame_ready()
        )
    );
    gtk_widget_queue_draw(state_->gl_area);
    gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
    if (state_->capture_parent != nullptr) {
        gtk_widget_queue_draw(state_->capture_parent);
    }
}

void WorkspaceOverviewMorphRenderer::finish() noexcept {
    if (state_ == nullptr) return;
    state_->lifecycle.finish();
    state_->source_upload_pending = false;
    state_->release_texture();
    state_->source_pixels.clear();
    state_->source_pixels.shrink_to_fit();
    state_->source_width = 0;
    state_->source_height = 0;
    state_->source_logical_width = 0;
    state_->source_logical_height = 0;
    state_->captured_source_bytes = 0;
    state_->clear_framebuffer();
    state_->allocation_wait_logged = false;
    state_->framebuffer_wait_logged = false;
    state_->failed = false;
    state_->capture_parent = nullptr;
    state_->source_child = nullptr;
    if (state_->gl_area != nullptr) {
        gtk_widget_set_opacity(state_->gl_area, 0.0);
        gtk_widget_set_size_request(state_->gl_area, -1, -1);
        gtk_widget_set_visible(state_->gl_area, FALSE);
    }
}

} // namespace realmheart::ui::workspace::animation
