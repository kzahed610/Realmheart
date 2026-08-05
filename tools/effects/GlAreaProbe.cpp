#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kProbeWidth = 720;
constexpr int kProbeHeight = 360;
constexpr int kSourceWidth = 646;
constexpr int kSourceHeight = 198;
constexpr double kVoidCycleSeconds = 2.6;

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

constexpr std::string_view kProbeCss = R"CSS(
window.realmheart-gl-probe-window,
.realmheart-gl-probe-underlay {
    background: linear-gradient(135deg,
        #211b36 0%,
        #44325f 42%,
        #796451 72%,
        #1c1728 100%);
}
.realmheart-gl-probe-underlay {
    border: 2px solid rgba(255, 214, 107, 0.72);
    border-radius: 24px;
}
.realmheart-gl-probe-underlay label {
    color: rgba(255, 255, 255, 0.88);
    font-size: 18px;
    font-weight: 700;
}
)CSS";

enum class ProbeMode {
    Solid,
    Void,
};

struct ProbeState {
    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;
    GtkGLArea* area = nullptr;
    GtkWidget* host = nullptr;
    bool layer_shell = false;
    ProbeMode mode = ProbeMode::Solid;
    unsigned render_count = 0;
    guint animation_source = 0;
    std::chrono::steady_clock::time_point animation_start{};

    GLuint program = 0;
    GLuint vertex_array = 0;
    GLuint source_texture = 0;
    std::vector<std::uint8_t> source_pixels;
    float progress = 0.0F;
};

void log_gl_error(GtkGLArea* area, std::string_view stage) {
    if (const GError* error = gtk_gl_area_get_error(area); error != nullptr) {
        std::cerr << "[Realmheart GL probe] " << stage
                  << " failed: " << error->message << '\n';
    }
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    );
}

GLuint compile_shader(GLenum type, std::string_view source, std::string& error) {
    const GLuint shader = glCreateShader(type);
    const char* source_pointer = source.data();
    const GLint source_length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &source_pointer, &source_length);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    error.assign(static_cast<std::size_t>(std::max(log_length, 1)), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, error.data());
    glDeleteShader(shader);
    return 0;
}

GLuint link_program(std::string_view fragment_source, std::string& error) {
    std::string vertex_error;
    const GLuint vertex = compile_shader(
        GL_VERTEX_SHADER,
        kVertexShader,
        vertex_error
    );
    if (vertex == 0) {
        error = "vertex shader compilation failed: " + vertex_error;
        return 0;
    }

    std::string fragment_error;
    const GLuint fragment = compile_shader(
        GL_FRAGMENT_SHADER,
        fragment_source,
        fragment_error
    );
    if (fragment == 0) {
        glDeleteShader(vertex);
        error = "fragment shader compilation failed: " + fragment_error;
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

    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    error.assign(static_cast<std::size_t>(std::max(log_length, 1)), '\0');
    glGetProgramInfoLog(program, log_length, nullptr, error.data());
    glDeleteProgram(program);
    return 0;
}

void set_pixel(
    std::vector<std::uint8_t>& pixels,
    int width,
    int x,
    int y,
    std::array<std::uint8_t, 4> rgba
) {
    const std::size_t offset = static_cast<std::size_t>((y * width + x) * 4);
    pixels[offset + 0] = rgba[0];
    pixels[offset + 1] = rgba[1];
    pixels[offset + 2] = rgba[2];
    pixels[offset + 3] = rgba[3];
}

bool inside_rounded_rect(
    float x,
    float y,
    float left,
    float top,
    float right,
    float bottom,
    float radius
) {
    const float clamped_x = std::clamp(x, left + radius, right - radius);
    const float clamped_y = std::clamp(y, top + radius, bottom - radius);
    const float dx = x - clamped_x;
    const float dy = y - clamped_y;
    return dx * dx + dy * dy <= radius * radius;
}

std::vector<std::uint8_t> make_launcher_texture() {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(kSourceWidth * kSourceHeight * 4),
        0
    );

    constexpr float panel_left = 3.0F;
    constexpr float panel_top = 3.0F;
    constexpr float panel_right = static_cast<float>(kSourceWidth - 3);
    constexpr float panel_bottom = static_cast<float>(kSourceHeight - 3);
    constexpr float panel_radius = 28.0F;

    for (int y = 0; y < kSourceHeight; ++y) {
        for (int x = 0; x < kSourceWidth; ++x) {
            const float fx = static_cast<float>(x) + 0.5F;
            const float fy = static_cast<float>(y) + 0.5F;
            if (!inside_rounded_rect(
                    fx,
                    fy,
                    panel_left,
                    panel_top,
                    panel_right,
                    panel_bottom,
                    panel_radius
                )) {
                continue;
            }

            const float edge = std::min({
                fx - panel_left,
                panel_right - fx,
                fy - panel_top,
                panel_bottom - fy,
            });
            const bool border = edge < 3.0F;
            const float vertical = fy / static_cast<float>(kSourceHeight);
            const float horizontal = fx / static_cast<float>(kSourceWidth);

            std::array<std::uint8_t, 4> colour{};
            if (border) {
                colour = {230, 196, 96, 255};
            } else {
                const float glow = 0.5F + 0.5F * std::sin(
                    horizontal * std::numbers::pi_v<float>
                );
                colour = {
                    static_cast<std::uint8_t>(25.0F + glow * 20.0F),
                    static_cast<std::uint8_t>(18.0F + vertical * 18.0F),
                    static_cast<std::uint8_t>(38.0F + glow * 30.0F),
                    255,
                };
            }
            set_pixel(pixels, kSourceWidth, x, y, colour);
        }
    }

    // Wallpaper aperture.
    for (int y = 20; y < 150; ++y) {
        for (int x = 26; x < kSourceWidth - 26; ++x) {
            const float t = static_cast<float>(y - 20) / 130.0F;
            const std::uint8_t shade = static_cast<std::uint8_t>(215.0F - t * 130.0F);
            set_pixel(
                pixels,
                kSourceWidth,
                x,
                y,
                {shade, static_cast<std::uint8_t>(shade - 8), static_cast<std::uint8_t>(shade - 18), 255}
            );
        }
    }

    // Search capsule.
    constexpr float search_left = 116.0F;
    constexpr float search_top = 58.0F;
    constexpr float search_right = 530.0F;
    constexpr float search_bottom = 116.0F;
    constexpr float search_radius = 29.0F;
    for (int y = 52; y < 122; ++y) {
        for (int x = 108; x < 538; ++x) {
            const float fx = static_cast<float>(x) + 0.5F;
            const float fy = static_cast<float>(y) + 0.5F;
            if (!inside_rounded_rect(
                    fx,
                    fy,
                    search_left,
                    search_top,
                    search_right,
                    search_bottom,
                    search_radius
                )) {
                continue;
            }
            const float edge = std::min({
                fx - search_left,
                search_right - fx,
                fy - search_top,
                search_bottom - fy,
            });
            set_pixel(
                pixels,
                kSourceWidth,
                x,
                y,
                edge < 2.0F
                    ? std::array<std::uint8_t, 4>{224, 191, 89, 255}
                    : std::array<std::uint8_t, 4>{24, 27, 35, 255}
            );
        }
    }

    return pixels;
}

void upload_source_texture(ProbeState& state) {
    if (state.source_pixels.empty()) {
        state.source_pixels = make_launcher_texture();
    }

    glGenTextures(1, &state.source_texture);
    glBindTexture(GL_TEXTURE_2D, state.source_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        kSourceWidth,
        kSourceHeight,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        state.source_pixels.data()
    );
    glBindTexture(GL_TEXTURE_2D, 0);
}

void release_gl_resources(ProbeState& state) {
    if (state.area == nullptr || !gtk_widget_get_realized(GTK_WIDGET(state.area))) {
        state.program = 0;
        state.vertex_array = 0;
        state.source_texture = 0;
        return;
    }

    gtk_gl_area_make_current(state.area);
    if (gtk_gl_area_get_error(state.area) != nullptr) return;

    if (state.source_texture != 0) glDeleteTextures(1, &state.source_texture);
    if (state.vertex_array != 0) glDeleteVertexArrays(1, &state.vertex_array);
    if (state.program != 0) glDeleteProgram(state.program);
    state.source_texture = 0;
    state.vertex_array = 0;
    state.program = 0;
}

void on_realize(GtkWidget* widget, gpointer user_data) {
    auto* state = static_cast<ProbeState*>(user_data);
    auto* area = GTK_GL_AREA(widget);
    std::cerr << "[Realmheart GL probe] realize\n";

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != nullptr) {
        log_gl_error(area, "context creation");
        return;
    }

    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* shading = reinterpret_cast<const char*>(
        glGetString(GL_SHADING_LANGUAGE_VERSION)
    );

    std::cerr << "[Realmheart GL probe] vendor="
              << (vendor != nullptr ? vendor : "unknown") << '\n';
    std::cerr << "[Realmheart GL probe] renderer="
              << (renderer != nullptr ? renderer : "unknown") << '\n';
    std::cerr << "[Realmheart GL probe] version="
              << (version != nullptr ? version : "unknown") << '\n';
    std::cerr << "[Realmheart GL probe] glsl="
              << (shading != nullptr ? shading : "unknown") << '\n';

    if (state->mode != ProbeMode::Void) return;

#ifndef REALMHEART_SOURCE_WINDOW_EFFECT_DIR
#error "REALMHEART_SOURCE_WINDOW_EFFECT_DIR must be defined for the GL probe"
#endif
    const auto shader_path = std::filesystem::path{REALMHEART_SOURCE_WINDOW_EFFECT_DIR} /
        "void" / "void.frag";
    const auto fragment_source = read_text_file(shader_path);
    if (!fragment_source) {
        std::cerr << "[Realmheart GL probe] unable to read "
                  << shader_path << '\n';
        return;
    }

    std::string error;
    state->program = link_program(*fragment_source, error);
    if (state->program == 0) {
        std::cerr << "[Realmheart GL probe] Realmheart Void link failed: "
                  << error << '\n';
        return;
    }

    glGenVertexArrays(1, &state->vertex_array);
    upload_source_texture(*state);
    std::cerr << "[Realmheart GL probe] Realmheart Void program and source texture ready\n";
}

void on_unrealize(GtkWidget*, gpointer user_data) {
    auto* state = static_cast<ProbeState*>(user_data);
    std::cerr << "[Realmheart GL probe] unrealize\n";
    release_gl_resources(*state);
}

void on_map(GtkWidget* widget, gpointer user_data) {
    auto* state = static_cast<ProbeState*>(user_data);
    std::cerr << "[Realmheart GL probe] map\n";
    state->animation_start = std::chrono::steady_clock::now();
    gtk_gl_area_queue_render(GTK_GL_AREA(widget));
}

void on_unmap(GtkWidget*, gpointer) {
    std::cerr << "[Realmheart GL probe] unmap\n";
}

void on_resize(GtkGLArea*, int width, int height, gpointer) {
    std::cerr << "[Realmheart GL probe] resize "
              << width << 'x' << height << '\n';
}

void clear_scissored_rect(
    int x,
    int y,
    int width,
    int height,
    const std::array<float, 4>& rgba
) {
    glScissor(x, y, std::max(width, 0), std::max(height, 0));
    glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}

void render_solid_probe(int width, int height) {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);

    clear_scissored_rect(
        0,
        0,
        width,
        height,
        {0.10F, 0.025F, 0.22F, 1.0F}
    );
    clear_scissored_rect(
        width / 14,
        height / 8,
        width * 5 / 14,
        height * 3 / 4,
        {0.35F, 0.10F, 0.68F, 1.0F}
    );
    clear_scissored_rect(
        width * 11 / 24,
        height * 5 / 16,
        width / 12,
        height * 3 / 8,
        {0.89F, 0.72F, 0.28F, 1.0F}
    );
    clear_scissored_rect(
        width * 8 / 14,
        height / 8,
        width * 5 / 14,
        height * 3 / 4,
        {0.50F, 0.30F, 0.88F, 1.0F}
    );

    glDisable(GL_SCISSOR_TEST);
}

void render_void_probe(ProbeState& state) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    if (state.program == 0 || state.source_texture == 0 ||
        state.vertex_array == 0) {
        return;
    }

    glUseProgram(state.program);
    glBindVertexArray(state.vertex_array);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, state.source_texture);

    glUniform1f(glGetUniformLocation(state.program, "progress"), state.progress);
    glUniform2f(
        glGetUniformLocation(state.program, "resolution"),
        static_cast<float>(kSourceWidth),
        static_cast<float>(kSourceHeight)
    );
    glUniform1i(glGetUniformLocation(state.program, "tex"), 0);
    glUniform1f(glGetUniformLocation(state.program, "radius"), 28.0F);
    glUniform1f(glGetUniformLocation(state.program, "reverse"), 0.0F);

    const std::array<float, 3> gold{0.886F, 0.725F, 0.416F};
    const std::array<float, 3> starlight{0.749F, 0.890F, 1.0F};
    const std::array<float, 3> astral{0.353F, 0.290F, 0.612F};
    const std::array<float, 3> void_colour{0.024F, 0.031F, 0.094F};
    glUniform3fv(glGetUniformLocation(state.program, "uGold"), 1, gold.data());
    glUniform3fv(
        glGetUniformLocation(state.program, "uStarlight"),
        1,
        starlight.data()
    );
    glUniform3fv(
        glGetUniformLocation(state.program, "uAstral"),
        1,
        astral.data()
    );
    glUniform3fv(
        glGetUniformLocation(state.program, "uVoid"),
        1,
        void_colour.data()
    );

    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

gboolean on_render(GtkGLArea* area, GdkGLContext*, gpointer user_data) {
    auto* state = static_cast<ProbeState*>(user_data);
    ++state->render_count;

    if (gtk_gl_area_get_error(area) != nullptr) {
        log_gl_error(area, "render");
        return TRUE;
    }

    const int logical_width = gtk_widget_get_width(GTK_WIDGET(area));
    const int logical_height = gtk_widget_get_height(GTK_WIDGET(area));
    const int scale = std::max(gtk_widget_get_scale_factor(GTK_WIDGET(area)), 1);
    const int width = std::max(logical_width * scale, 1);
    const int height = std::max(logical_height * scale, 1);

    if (state->render_count <= 3 || state->render_count % 60 == 0) {
        std::cerr << "[Realmheart GL probe] render #" << state->render_count
                  << " logical=" << logical_width << 'x' << logical_height
                  << " framebuffer=" << width << 'x' << height;
        if (state->mode == ProbeMode::Void) {
            std::cerr << " progress=" << state->progress;
        }
        std::cerr << '\n';
    }

    glViewport(0, 0, width, height);
    if (state->mode == ProbeMode::Void) {
        render_void_probe(*state);
    } else {
        render_solid_probe(width, height);
    }

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::cerr << "[Realmheart GL probe] OpenGL error after draw: 0x"
                  << std::hex << static_cast<unsigned>(error)
                  << std::dec << '\n';
    }

    return TRUE;
}

gboolean animate_void(gpointer user_data) {
    auto* state = static_cast<ProbeState*>(user_data);
    if (state->area == nullptr ||
        !gtk_widget_get_mapped(GTK_WIDGET(state->area))) {
        return G_SOURCE_CONTINUE;
    }

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - state->animation_start
    ).count();
    const double cycle = std::fmod(elapsed, kVoidCycleSeconds) / kVoidCycleSeconds;
    const double ping_pong = cycle < 0.5
        ? cycle * 2.0
        : (1.0 - cycle) * 2.0;
    state->progress = static_cast<float>(ping_pong);
    gtk_gl_area_queue_render(state->area);
    return G_SOURCE_CONTINUE;
}

gboolean queue_second_frame(gpointer user_data) {
    auto* state = static_cast<ProbeState*>(user_data);
    if (state->area != nullptr &&
        gtk_widget_get_mapped(GTK_WIDGET(state->area))) {
        std::cerr << "[Realmheart GL probe] queue second frame\n";
        gtk_gl_area_queue_render(state->area);
    }
    return G_SOURCE_REMOVE;
}

gboolean on_key_pressed(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType,
    gpointer user_data
) {
    auto* state = static_cast<ProbeState*>(user_data);

    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(state->window);
        return TRUE;
    }

    if (keyval == GDK_KEY_space || keyval == GDK_KEY_r || keyval == GDK_KEY_R) {
        std::cerr << "[Realmheart GL probe] manual repaint request\n";
        if (state->mode == ProbeMode::Void) {
            state->animation_start = std::chrono::steady_clock::now();
        }
        gtk_gl_area_queue_render(state->area);
        return TRUE;
    }

    return FALSE;
}

void configure_layer_shell(GtkWindow* window) {
    gtk_window_set_decorated(window, FALSE);
    gtk_layer_init_for_window(window);
    gtk_layer_set_namespace(window, "realmheart-gl-probe");
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(
        window,
        GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND
    );
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, 120);
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_LEFT, 120);
    gtk_layer_set_exclusive_zone(window, 0);
}

void install_probe_css() {
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, kProbeCss.data());
    GdkDisplay* display = gdk_display_get_default();
    if (display != nullptr) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
    }
    g_object_unref(provider);
}

GtkWidget* build_probe_host(ProbeState& state) {
    if (state.mode != ProbeMode::Void) {
        return GTK_WIDGET(state.area);
    }

    GtkWidget* overlay = gtk_overlay_new();
    gtk_widget_set_size_request(overlay, kProbeWidth, kProbeHeight);

    GtkWidget* underlay = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(underlay, "realmheart-gl-probe-underlay");
    gtk_widget_set_hexpand(underlay, TRUE);
    gtk_widget_set_vexpand(underlay, TRUE);

    GtkWidget* label = gtk_label_new(
        "Desktop/GTK content beneath the transparent Realmheart Void"
    );
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_vexpand(label, TRUE);
    gtk_box_append(GTK_BOX(underlay), label);

    gtk_overlay_set_child(GTK_OVERLAY(overlay), underlay);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(state.area));
    gtk_overlay_set_clip_overlay(
        GTK_OVERLAY(overlay),
        GTK_WIDGET(state.area),
        TRUE
    );
    return overlay;
}

void on_activate(GtkApplication* application, gpointer user_data) {
    auto* state = static_cast<ProbeState*>(user_data);
    state->application = application;

    install_probe_css();

    state->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(state->window, "Realmheart GtkGLArea Probe");
    gtk_window_set_default_size(state->window, kProbeWidth, kProbeHeight);
    gtk_window_set_resizable(state->window, FALSE);
    gtk_widget_add_css_class(
        GTK_WIDGET(state->window),
        "realmheart-gl-probe-window"
    );

    if (state->layer_shell) {
        configure_layer_shell(state->window);
    }

    state->area = GTK_GL_AREA(gtk_gl_area_new());
    gtk_widget_set_hexpand(GTK_WIDGET(state->area), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(state->area), TRUE);
    gtk_widget_set_size_request(
        GTK_WIDGET(state->area),
        kProbeWidth,
        kProbeHeight
    );
    gtk_gl_area_set_allowed_apis(state->area, GDK_GL_API_GLES);
    gtk_gl_area_set_required_version(state->area, 3, 0);
    gtk_gl_area_set_has_depth_buffer(state->area, FALSE);
    gtk_gl_area_set_has_stencil_buffer(state->area, FALSE);
    gtk_gl_area_set_auto_render(state->area, FALSE);

    g_signal_connect(state->area, "realize", G_CALLBACK(on_realize), state);
    g_signal_connect(state->area, "unrealize", G_CALLBACK(on_unrealize), state);
    g_signal_connect(state->area, "map", G_CALLBACK(on_map), state);
    g_signal_connect(state->area, "unmap", G_CALLBACK(on_unmap), state);
    g_signal_connect(state->area, "resize", G_CALLBACK(on_resize), state);
    g_signal_connect(state->area, "render", G_CALLBACK(on_render), state);

    auto* key_controller = gtk_event_controller_key_new();
    g_signal_connect(
        key_controller,
        "key-pressed",
        G_CALLBACK(on_key_pressed),
        state
    );
    gtk_widget_add_controller(
        GTK_WIDGET(state->window),
        key_controller
    );

    state->host = build_probe_host(*state);
    gtk_window_set_child(state->window, state->host);
    gtk_window_present(state->window);

    std::cerr << "[Realmheart GL probe] presented "
              << (state->layer_shell ? "layer-shell" : "normal")
              << " window in "
              << (state->mode == ProbeMode::Void ? "Realmheart Void" : "solid")
              << " mode\n";
    std::cerr << "[Realmheart GL probe] press Space/R to restart/repaint; Esc to close\n";

    if (state->mode == ProbeMode::Void) {
        state->animation_source = g_timeout_add(16, animate_void, state);
    } else {
        g_timeout_add(750, queue_second_frame, state);
    }
}

bool has_argument(int argc, char** argv, std::string_view target) {
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && target == argv[index]) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    ProbeState state;
    state.layer_shell = has_argument(argc, argv, "--layer-shell");
    state.mode = has_argument(argc, argv, "--void")
        ? ProbeMode::Void
        : ProbeMode::Solid;

    GtkApplication* application = gtk_application_new(
        "io.realmheart.GLProbe",
        G_APPLICATION_NON_UNIQUE
    );
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), &state);

    std::vector<char*> forwarded_arguments;
    forwarded_arguments.reserve(static_cast<std::size_t>(argc) + 1U);
    if (argc > 0) forwarded_arguments.push_back(argv[0]);
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) continue;
        const std::string_view argument{argv[index]};
        if (argument == "--layer-shell" || argument == "--void") continue;
        forwarded_arguments.push_back(argv[index]);
    }
    forwarded_arguments.push_back(nullptr);

    const int result = g_application_run(
        G_APPLICATION(application),
        static_cast<int>(forwarded_arguments.size() - 1U),
        forwarded_arguments.data()
    );

    if (state.animation_source != 0) {
        g_source_remove(state.animation_source);
        state.animation_source = 0;
    }
    g_object_unref(application);
    return result;
}
