#include "wallpaper-native/NativeWallpaperRenderer.hpp"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <poll.h>
#include <string_view>
#include <unistd.h>

namespace realmheart::wallpaper_native {

namespace {

constexpr char kVertexShader[] = R"(
attribute vec2 a_position;
attribute vec2 a_uv;
varying vec2 v_uv;
void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

constexpr char kFragmentShader[] = R"(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_current;
uniform sampler2D u_next;
uniform float u_progress;
uniform vec4 u_current_uv;
uniform vec4 u_next_uv;

vec2 crop_uv(vec4 rect, vec2 base_uv) {
    return mix(rect.xy, rect.zw, base_uv);
}

void main() {
    vec4 from_color = texture2D(u_current, crop_uv(u_current_uv, v_uv));
    vec4 to_color = texture2D(u_next, crop_uv(u_next_uv, v_uv));
    gl_FragColor = mix(from_color, to_color, u_progress);
}
)";

constexpr wl_registry_listener kRegistryListener{
    &NativeWallpaperRenderer::registry_global,
    &NativeWallpaperRenderer::registry_global_remove,
};

constexpr wl_output_listener kOutputListener{
    &NativeWallpaperRenderer::output_geometry,
    &NativeWallpaperRenderer::output_mode,
    &NativeWallpaperRenderer::output_done,
    &NativeWallpaperRenderer::output_scale,
    &NativeWallpaperRenderer::output_name,
    &NativeWallpaperRenderer::output_description,
};

constexpr zwlr_layer_surface_v1_listener kLayerSurfaceListener{
    &NativeWallpaperRenderer::layer_surface_configure,
    &NativeWallpaperRenderer::layer_surface_closed,
};

void set_error(std::string* destination, const std::string& message) {
    if (destination != nullptr) *destination = message;
}

std::array<float, 4> cover_uv(
    int image_width,
    int image_height,
    int surface_width,
    int surface_height
) {
    if (image_width <= 0 || image_height <= 0 ||
        surface_width <= 0 || surface_height <= 0) {
        return {0.0F, 1.0F, 1.0F, 0.0F};
    }

    const float image_aspect = static_cast<float>(image_width) /
                               static_cast<float>(image_height);
    const float surface_aspect = static_cast<float>(surface_width) /
                                 static_cast<float>(surface_height);

    float left = 0.0F;
    float right = 1.0F;
    float top = 0.0F;
    float bottom = 1.0F;

    if (image_aspect > surface_aspect) {
        const float visible_width = surface_aspect / image_aspect;
        left = (1.0F - visible_width) * 0.5F;
        right = left + visible_width;
    } else if (image_aspect < surface_aspect) {
        const float visible_height = image_aspect / surface_aspect;
        top = (1.0F - visible_height) * 0.5F;
        bottom = top + visible_height;
    }

    // Pixbuf rows begin at the top; OpenGL's V=0 samples the first uploaded row.
    // Reverse V so the image is displayed upright on screen.
    return {left, bottom, right, top};
}

std::string base64_encode(std::string_view value) {
    gchar* encoded = g_base64_encode(
        reinterpret_cast<const guchar*>(value.data()),
        value.size()
    );
    if (encoded == nullptr) return {};
    std::string result = encoded;
    g_free(encoded);
    return result;
}

} // namespace

NativeWallpaperRenderer::~NativeWallpaperRenderer() {
    cleanup();
}

bool NativeWallpaperRenderer::initialize(std::string* error_message) {
    if (error_message != nullptr) error_message->clear();
    if (initialized_) return true;

    if (!initialize_wayland(error_message) ||
        !initialize_egl(error_message) ||
        !initialize_gl(error_message)) {
        cleanup();
        return false;
    }

    for (const auto& output : outputs_) {
        if (!create_output_surface(*output, error_message)) {
            cleanup();
            return false;
        }
    }

    if (wl_display_roundtrip(display_) < 0) {
        set_error(error_message, "Wayland failed while configuring wallpaper surfaces");
        cleanup();
        return false;
    }

    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }

    initialized_ = true;
    return true;
}

bool NativeWallpaperRenderer::initialize_wayland(std::string* error_message) {
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        set_error(error_message, "unable to connect to the Wayland compositor");
        return false;
    }

    registry_ = wl_display_get_registry(display_);
    if (registry_ == nullptr) {
        set_error(error_message, "unable to obtain the Wayland registry");
        return false;
    }

    wl_registry_add_listener(registry_, &kRegistryListener, this);
    if (wl_display_roundtrip(display_) < 0) {
        set_error(error_message, "Wayland registry roundtrip failed");
        return false;
    }

    if (compositor_ == nullptr) {
        set_error(error_message, "Wayland compositor global is unavailable");
        return false;
    }
    if (layer_shell_ == nullptr) {
        set_error(error_message, "wlr-layer-shell is unavailable");
        return false;
    }
    if (outputs_.empty()) {
        set_error(error_message, "Wayland compositor exposed no outputs");
        return false;
    }
    return true;
}

bool NativeWallpaperRenderer::initialize_egl(std::string* error_message) {
    egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
    if (egl_display_ == EGL_NO_DISPLAY) {
        set_error(error_message, egl_error_message("eglGetDisplay"));
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(egl_display_, &major, &minor) != EGL_TRUE) {
        set_error(error_message, egl_error_message("eglInitialize"));
        return false;
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        set_error(error_message, egl_error_message("eglBindAPI"));
        return false;
    }

    constexpr EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };

    EGLint config_count = 0;
    if (eglChooseConfig(
            egl_display_,
            config_attributes,
            &egl_config_,
            1,
            &config_count
        ) != EGL_TRUE || config_count == 0) {
        set_error(error_message, egl_error_message("eglChooseConfig"));
        return false;
    }

    constexpr EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    egl_context_ = eglCreateContext(
        egl_display_,
        egl_config_,
        EGL_NO_CONTEXT,
        context_attributes
    );
    if (egl_context_ == EGL_NO_CONTEXT) {
        set_error(error_message, egl_error_message("eglCreateContext"));
        return false;
    }

    constexpr EGLint pbuffer_attributes[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE,
    };
    pbuffer_surface_ = eglCreatePbufferSurface(
        egl_display_,
        egl_config_,
        pbuffer_attributes
    );
    if (pbuffer_surface_ == EGL_NO_SURFACE) {
        set_error(error_message, egl_error_message("eglCreatePbufferSurface"));
        return false;
    }

    return make_pbuffer_current(error_message);
}

bool NativeWallpaperRenderer::initialize_gl(std::string* error_message) {
    if (!make_pbuffer_current(error_message)) return false;
    if (!link_program(error_message)) return false;

    position_attribute_ = glGetAttribLocation(program_, "a_position");
    uv_attribute_ = glGetAttribLocation(program_, "a_uv");
    current_sampler_uniform_ = glGetUniformLocation(program_, "u_current");
    next_sampler_uniform_ = glGetUniformLocation(program_, "u_next");
    progress_uniform_ = glGetUniformLocation(program_, "u_progress");
    current_uv_uniform_ = glGetUniformLocation(program_, "u_current_uv");
    next_uv_uniform_ = glGetUniformLocation(program_, "u_next_uv");

    if (position_attribute_ < 0 || uv_attribute_ < 0 ||
        current_sampler_uniform_ < 0 || next_sampler_uniform_ < 0 ||
        progress_uniform_ < 0 || current_uv_uniform_ < 0 ||
        next_uv_uniform_ < 0) {
        set_error(error_message, "native wallpaper shader interface is incomplete");
        return false;
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    return true;
}

bool NativeWallpaperRenderer::set_wallpaper(
    const std::string& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_) {
        set_error(error_message, "native wallpaper renderer is not initialized");
        return false;
    }

    discard_prepared_wallpaper();

    Texture candidate;
    if (!upload_texture(path, candidate, error_message)) return false;

    if (current_texture_.id == 0) {
        clear_output_overrides();
        current_texture_ = candidate;
        draw_all();
        return true;
    }

    destroy_texture(next_texture_);
    next_texture_ = candidate;
    active_transition_duration_ = transition_duration_;
    animation_started_ = std::chrono::steady_clock::now();
    animating_ = true;
    draw_all();
    return true;
}

bool NativeWallpaperRenderer::prepare_wallpaper(
    const std::string& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_) {
        set_error(error_message, "native wallpaper renderer is not initialized");
        return false;
    }
    if (animating_) {
        set_error(error_message, "native wallpaper renderer is still transitioning");
        return false;
    }

    // PREPARE is authoritative for the next transaction. Never retain a stale
    // full-resolution candidate if this decode fails or the selection changed.
    discard_prepared_wallpaper();
    Texture candidate;
    if (!upload_texture(path, candidate, error_message)) return false;
    prepared_texture_ = candidate;
    prepared_output_name_.clear();
    return true;
}

bool NativeWallpaperRenderer::prepare_wallpaper_for_output(
    const std::string& path,
    const std::string& output_name,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_) {
        set_error(error_message, "native wallpaper renderer is not initialized");
        return false;
    }
    if (animating_) {
        set_error(error_message, "native wallpaper renderer is still transitioning");
        return false;
    }
    if (output_name.empty() || find_output_by_name(output_name) == nullptr) {
        set_error(
            error_message,
            "native wallpaper output is unavailable: " + output_name
        );
        return false;
    }

    discard_prepared_wallpaper();
    Texture candidate;
    if (!upload_texture(path, candidate, error_message, output_name)) return false;
    prepared_texture_ = candidate;
    prepared_output_name_ = output_name;
    return true;
}

bool NativeWallpaperRenderer::commit_prepared_wallpaper(
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_) {
        set_error(error_message, "native wallpaper renderer is not initialized");
        return false;
    }
    if (prepared_texture_.id == 0) {
        set_error(error_message, "native wallpaper renderer has no prepared wallpaper");
        return false;
    }
    if (animating_) {
        set_error(error_message, "native wallpaper renderer is already transitioning");
        return false;
    }

    if (!prepared_output_name_.empty()) {
        OutputSurface* output = find_output_by_name(prepared_output_name_);
        if (output == nullptr) {
            set_error(
                error_message,
                "native wallpaper output disappeared before commit: " +
                    prepared_output_name_
            );
            discard_prepared_wallpaper();
            return false;
        }
        if (!make_pbuffer_current(error_message)) return false;

        destroy_texture(output->override_texture);
        output->override_texture = prepared_texture_;
        prepared_texture_ = {};
        prepared_output_name_.clear();
        if (output->configured && !output->closed) {
            draw_output(*output, 1.0F);
        }
        return true;
    }

    if (current_texture_.id == 0) {
        clear_output_overrides();
        current_texture_ = prepared_texture_;
        prepared_texture_ = {};
        draw_all();
        return true;
    }

    destroy_texture(next_texture_);
    next_texture_ = prepared_texture_;
    prepared_texture_ = {};
    active_transition_duration_ = transition_duration_;
    animation_started_ = std::chrono::steady_clock::now();
    animating_ = true;
    draw_all();
    return true;
}

void NativeWallpaperRenderer::discard_prepared_wallpaper() noexcept {
    prepared_output_name_.clear();
    if (!initialized_ || prepared_texture_.id == 0) return;
    if (!make_pbuffer_current()) return;
    destroy_texture(prepared_texture_);
}

int NativeWallpaperRenderer::run_stdio() {
    if (!initialized_) return 1;
    running_ = true;

    while (running_) {
        if (wl_display_dispatch_pending(display_) < 0) return 1;
        if (wl_display_flush(display_) < 0 && errno != EAGAIN) return 1;

        pollfd descriptors[2] = {
            {wl_display_get_fd(display_), POLLIN, 0},
            {STDIN_FILENO, POLLIN | POLLHUP, 0},
        };

        const int timeout = animating_ ? 16 : -1;
        const int result = poll(descriptors, 2, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return 1;
        }

        if ((descriptors[0].revents & POLLIN) != 0) {
            if (wl_display_dispatch(display_) < 0) return 1;
        }

        if ((descriptors[1].revents & POLLIN) != 0) {
            process_stdin_bytes();
        }
        if ((descriptors[1].revents & POLLHUP) != 0) {
            running_ = false;
        }

        if (animating_) advance_animation();
    }

    return 0;
}

void NativeWallpaperRenderer::registry_global(
    void* data,
    wl_registry* registry,
    std::uint32_t name,
    const char* interface,
    std::uint32_t version
) {
    auto* self = static_cast<NativeWallpaperRenderer*>(data);

    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        const std::uint32_t bind_version = std::min(version, 4U);
        self->compositor_ = static_cast<wl_compositor*>(wl_registry_bind(
            registry,
            name,
            &wl_compositor_interface,
            bind_version
        ));
        return;
    }

    if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        const std::uint32_t bind_version = std::min(version, 4U);
        self->layer_shell_ = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(
            registry,
            name,
            &zwlr_layer_shell_v1_interface,
            bind_version
        ));
        return;
    }

    if (std::strcmp(interface, wl_output_interface.name) == 0) {
        auto output = std::make_unique<OutputSurface>();
        output->owner = self;
        output->registry_name = name;
        const std::uint32_t bind_version = std::min(version, 4U);
        output->output = static_cast<wl_output*>(wl_registry_bind(
            registry,
            name,
            &wl_output_interface,
            bind_version
        ));
        wl_output_add_listener(output->output, &kOutputListener, output.get());

        OutputSurface* raw_output = output.get();
        self->outputs_.push_back(std::move(output));
        if (self->initialized_) {
            std::string error;
            if (!self->create_output_surface(*raw_output, &error)) {
                std::cerr << "Unable to create wallpaper surface: " << error << '\n';
            }
        }
    }
}

void NativeWallpaperRenderer::registry_global_remove(
    void* data,
    wl_registry* /*registry*/,
    std::uint32_t name
) {
    static_cast<NativeWallpaperRenderer*>(data)->remove_output(name);
}

void NativeWallpaperRenderer::output_geometry(
    void*, wl_output*, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
    std::int32_t, const char*, const char*, std::int32_t
) {}

void NativeWallpaperRenderer::output_mode(
    void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t
) {}

void NativeWallpaperRenderer::output_done(void*, wl_output*) {}

void NativeWallpaperRenderer::output_scale(
    void* data,
    wl_output* /*output*/,
    std::int32_t factor
) {
    auto* surface = static_cast<OutputSurface*>(data);
    surface->scale = std::max(factor, 1);
    if (surface->surface != nullptr) {
        wl_surface_set_buffer_scale(surface->surface, surface->scale);
    }
    if (surface->owner->initialized_ && surface->configured) {
        std::string error;
        if (surface->owner->configure_egl_surface(
                *surface,
                surface->logical_width,
                surface->logical_height,
                &error
            )) {
            surface->owner->draw_all();
        } else {
            std::cerr << "Unable to apply output scale to wallpaper: "
                      << error << '\n';
        }
    }
}

void NativeWallpaperRenderer::output_name(
    void* data,
    wl_output*,
    const char* name
) {
    auto* surface = static_cast<OutputSurface*>(data);
    surface->name = name != nullptr ? name : "";
}
void NativeWallpaperRenderer::output_description(void*, wl_output*, const char*) {}

void NativeWallpaperRenderer::layer_surface_configure(
    void* data,
    zwlr_layer_surface_v1* layer_surface,
    std::uint32_t serial,
    std::uint32_t width,
    std::uint32_t height
) {
    auto* output = static_cast<OutputSurface*>(data);
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    const int configured_width = static_cast<int>(width);
    const int configured_height = static_cast<int>(height);
    if (configured_width <= 0 || configured_height <= 0) return;

    std::string error;
    if (!output->owner->configure_egl_surface(
            *output,
            configured_width,
            configured_height,
            &error
        )) {
        std::cerr << "Unable to configure wallpaper EGL surface: " << error << '\n';
        return;
    }

    output->configured = true;
    output->owner->draw_output(
        *output,
        output->owner->animating_ ? 0.0F : 1.0F
    );
}

void NativeWallpaperRenderer::layer_surface_closed(
    void* data,
    zwlr_layer_surface_v1* /*layer_surface*/
) {
    auto* output = static_cast<OutputSurface*>(data);
    output->closed = true;
}

bool NativeWallpaperRenderer::create_output_surface(
    OutputSurface& output,
    std::string* error_message
) {
    if (output.surface != nullptr) return true;
    if (compositor_ == nullptr || layer_shell_ == nullptr || output.output == nullptr) {
        set_error(error_message, "Wayland globals are incomplete");
        return false;
    }

    output.surface = wl_compositor_create_surface(compositor_);
    if (output.surface == nullptr) {
        set_error(error_message, "unable to create Wayland wallpaper surface");
        return false;
    }

    wl_surface_set_buffer_scale(output.surface, output.scale);
    wl_region* empty_region = wl_compositor_create_region(compositor_);
    if (empty_region != nullptr) {
        wl_surface_set_input_region(output.surface, empty_region);
        wl_region_destroy(empty_region);
    }

    output.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell_,
        output.surface,
        output.output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        "realmheart-wallpaper"
    );
    if (output.layer_surface == nullptr) {
        set_error(error_message, "unable to create layer-shell wallpaper surface");
        return false;
    }

    zwlr_layer_surface_v1_add_listener(
        output.layer_surface,
        &kLayerSurfaceListener,
        &output
    );
    zwlr_layer_surface_v1_set_size(output.layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(
        output.layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
    );
    zwlr_layer_surface_v1_set_exclusive_zone(output.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        output.layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
    );
    wl_surface_commit(output.surface);
    return true;
}

bool NativeWallpaperRenderer::configure_egl_surface(
    OutputSurface& output,
    int width,
    int height,
    std::string* error_message
) {
    output.logical_width = width;
    output.logical_height = height;
    const int pixel_width = std::max(width * output.scale, 1);
    const int pixel_height = std::max(height * output.scale, 1);

    if (output.egl_window == nullptr) {
        output.egl_window = wl_egl_window_create(
            output.surface,
            pixel_width,
            pixel_height
        );
        if (output.egl_window == nullptr) {
            set_error(error_message, "wl_egl_window_create failed");
            return false;
        }

        output.egl_surface = eglCreateWindowSurface(
            egl_display_,
            egl_config_,
            reinterpret_cast<EGLNativeWindowType>(output.egl_window),
            nullptr
        );
        if (output.egl_surface == EGL_NO_SURFACE) {
            set_error(error_message, egl_error_message("eglCreateWindowSurface"));
            return false;
        }
    } else {
        wl_egl_window_resize(output.egl_window, pixel_width, pixel_height, 0, 0);
    }

    return true;
}

bool NativeWallpaperRenderer::upload_texture(
    const std::string& path,
    Texture& texture,
    std::string* error_message,
    std::string_view target_output
) {
    if (!make_pbuffer_current(error_message)) return false;

    int source_width = 0;
    int source_height = 0;
    (void)gdk_pixbuf_get_file_info(
        path.c_str(),
        &source_width,
        &source_height
    );

    int target_width = source_width;
    int target_height = source_height;
    if (source_width > 0 && source_height > 0) {
        double required_scale = 0.0;
        for (const auto& output : outputs_) {
            if (!output->configured || output->closed) continue;
            if (!target_output.empty() && output->name != target_output) continue;
            const int output_width = std::max(
                output->logical_width * output->scale,
                1
            );
            const int output_height = std::max(
                output->logical_height * output->scale,
                1
            );
            required_scale = std::max(
                required_scale,
                std::max(
                    static_cast<double>(output_width) / source_width,
                    static_cast<double>(output_height) / source_height
                )
            );
        }

        // Never upscale a source during decoding. When downscaling, preserve
        // enough pixels for COVER on every configured output.
        if (required_scale > 0.0 && required_scale < 1.0) {
            target_width = std::max(
                1,
                static_cast<int>(std::ceil(source_width * required_scale))
            );
            target_height = std::max(
                1,
                static_cast<int>(std::ceil(source_height * required_scale))
            );
        }

        GLint maximum_texture_size = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
        if (maximum_texture_size > 0 &&
            (target_width > maximum_texture_size ||
             target_height > maximum_texture_size)) {
            const double limit_scale = std::min(
                static_cast<double>(maximum_texture_size) / target_width,
                static_cast<double>(maximum_texture_size) / target_height
            );
            target_width = std::max(
                1,
                static_cast<int>(std::floor(target_width * limit_scale))
            );
            target_height = std::max(
                1,
                static_cast<int>(std::floor(target_height * limit_scale))
            );
        }
    }

    GError* error = nullptr;
    GdkPixbuf* pixbuf = nullptr;
    if (source_width > 0 && source_height > 0 &&
        (target_width != source_width || target_height != source_height)) {
        pixbuf = gdk_pixbuf_new_from_file_at_scale(
            path.c_str(),
            target_width,
            target_height,
            TRUE,
            &error
        );
    } else {
        pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &error);
    }

    if (pixbuf == nullptr) {
        set_error(
            error_message,
            error != nullptr ? error->message : "unable to decode wallpaper"
        );
        if (error != nullptr) g_error_free(error);
        return false;
    }
    if (error != nullptr) g_error_free(error);

    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int row_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar* source = gdk_pixbuf_read_pixels(pixbuf);

    if (width <= 0 || height <= 0 || source == nullptr ||
        (channels != 3 && channels != 4)) {
        g_object_unref(pixbuf);
        set_error(error_message, "decoded wallpaper has an unsupported pixel layout");
        return false;
    }

    const std::size_t tight_stride = static_cast<std::size_t>(width) * channels;
    const GLenum format = channels == 4 ? GL_RGBA : GL_RGB;

    // Clear any stale error left by driver initialization before attributing an
    // upload failure to this texture.
    while (glGetError() != GL_NO_ERROR) {}

    glGenTextures(1, &texture.id);
    if (texture.id == 0) {
        g_object_unref(pixbuf);
        set_error(error_message, "OpenGL failed to allocate a wallpaper texture");
        return false;
    }

    texture.width = width;
    texture.height = height;
    glBindTexture(GL_TEXTURE_2D, texture.id);
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
    g_object_unref(pixbuf);

    if (gl_error != GL_NO_ERROR) {
        destroy_texture(texture);
        set_error(error_message, "OpenGL failed while uploading the wallpaper texture");
        return false;
    }

    return true;
}

GLuint NativeWallpaperRenderer::compile_shader(
    GLenum type,
    const char* source,
    std::string* error_message
) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        set_error(error_message, "OpenGL could not create a shader");
        return 0;
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    set_error(error_message, "wallpaper shader compilation failed: " + log);
    return 0;
}

bool NativeWallpaperRenderer::link_program(std::string* error_message) {
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader, error_message);
    if (vertex == 0) return false;

    const GLuint fragment = compile_shader(
        GL_FRAGMENT_SHADER,
        kFragmentShader,
        error_message
    );
    if (fragment == 0) {
        glDeleteShader(vertex);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint status = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &status);
    if (status == GL_TRUE) return true;

    GLint length = 0;
    glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program_, length, nullptr, log.data());
    set_error(error_message, "wallpaper shader linking failed: " + log);
    glDeleteProgram(program_);
    program_ = 0;
    return false;
}

void NativeWallpaperRenderer::draw_all() {
    float progress = 1.0F;
    if (animating_) {
        const auto elapsed = std::chrono::steady_clock::now() - animation_started_;
        progress = std::clamp(
            std::chrono::duration<float, std::milli>(elapsed).count() /
                static_cast<float>(active_transition_duration_.count()),
            0.0F,
            1.0F
        );
    }

    for (const auto& output : outputs_) {
        if (output->configured && !output->closed) {
            draw_output(*output, progress);
        }
    }
}

void NativeWallpaperRenderer::draw_output(OutputSurface& output, float progress) {
    const Texture& current = output.override_texture.id != 0
        ? output.override_texture
        : current_texture_;
    if (output.egl_surface == EGL_NO_SURFACE || current.id == 0) return;

    if (eglMakeCurrent(
            egl_display_,
            output.egl_surface,
            output.egl_surface,
            egl_context_
        ) != EGL_TRUE) {
        return;
    }

    const int pixel_width = std::max(output.logical_width * output.scale, 1);
    const int pixel_height = std::max(output.logical_height * output.scale, 1);
    glViewport(0, 0, pixel_width, pixel_height);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    constexpr std::array<float, 8> positions{
        -1.0F, -1.0F,
         1.0F, -1.0F,
        -1.0F,  1.0F,
         1.0F,  1.0F,
    };
    constexpr std::array<float, 8> uvs{
        0.0F, 0.0F,
        1.0F, 0.0F,
        0.0F, 1.0F,
        1.0F, 1.0F,
    };

    const Texture& incoming = next_texture_.id != 0 ? next_texture_ : current;
    const auto current_uv = cover_uv(
        current.width,
        current.height,
        pixel_width,
        pixel_height
    );
    const auto next_uv = cover_uv(
        incoming.width,
        incoming.height,
        pixel_width,
        pixel_height
    );

    glUseProgram(program_);
    glEnableVertexAttribArray(static_cast<GLuint>(position_attribute_));
    glEnableVertexAttribArray(static_cast<GLuint>(uv_attribute_));
    glVertexAttribPointer(
        static_cast<GLuint>(position_attribute_),
        2,
        GL_FLOAT,
        GL_FALSE,
        0,
        positions.data()
    );
    glVertexAttribPointer(
        static_cast<GLuint>(uv_attribute_),
        2,
        GL_FLOAT,
        GL_FALSE,
        0,
        uvs.data()
    );

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, current.id);
    glUniform1i(current_sampler_uniform_, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, incoming.id);
    glUniform1i(next_sampler_uniform_, 1);

    glUniform1f(progress_uniform_, next_texture_.id != 0 ? progress : 1.0F);
    glUniform4fv(current_uv_uniform_, 1, current_uv.data());
    glUniform4fv(next_uv_uniform_, 1, next_uv.data());

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(static_cast<GLuint>(position_attribute_));
    glDisableVertexAttribArray(static_cast<GLuint>(uv_attribute_));
    glBindTexture(GL_TEXTURE_2D, 0);
    eglSwapBuffers(egl_display_, output.egl_surface);
}

NativeWallpaperRenderer::OutputSurface*
NativeWallpaperRenderer::find_output_by_name(std::string_view name) noexcept {
    if (name.empty()) return nullptr;
    const auto found = std::find_if(
        outputs_.begin(),
        outputs_.end(),
        [name](const auto& output) {
            return output != nullptr && output->name == name;
        }
    );
    return found != outputs_.end() ? found->get() : nullptr;
}

void NativeWallpaperRenderer::clear_output_overrides() noexcept {
    for (const auto& output : outputs_) {
        if (output != nullptr) destroy_texture(output->override_texture);
    }
}

void NativeWallpaperRenderer::advance_animation() {
    const auto elapsed = std::chrono::steady_clock::now() - animation_started_;
    const bool complete = elapsed >= active_transition_duration_;
    draw_all();

    if (!complete) return;

    if (!make_pbuffer_current()) return;
    clear_output_overrides();
    destroy_texture(current_texture_);
    current_texture_ = next_texture_;
    next_texture_ = {};
    animating_ = false;
    active_transition_duration_ = transition_duration_;
    draw_all();

    // SET / COMMIT is acknowledged only after the final wallpaper frame has
    // been submitted. Callers get a real visual-ready boundary instead of a
    // guessed sleep.
    if (set_response_pending_) {
        set_response_pending_ = false;
        // Synchronize once after the final buffer commit so the acknowledgement
        // cannot overtake that commit on the Wayland connection. This is a
        // protocol readiness boundary, not a timing guess.
        if (wl_display_roundtrip(display_) < 0) {
            send_error("Wayland failed while confirming final wallpaper frame");
            running_ = false;
            return;
        }
        send_ok();
    }
}

void NativeWallpaperRenderer::destroy_texture(Texture& texture) noexcept {
    if (texture.id != 0) {
        glDeleteTextures(1, &texture.id);
    }
    texture = {};
}

void NativeWallpaperRenderer::destroy_output_surface(OutputSurface& output) noexcept {
    if (initialized_) {
        (void)make_pbuffer_current();
        destroy_texture(output.override_texture);
    }
    if (output.egl_surface != EGL_NO_SURFACE && egl_display_ != EGL_NO_DISPLAY) {
        eglDestroySurface(egl_display_, output.egl_surface);
        output.egl_surface = EGL_NO_SURFACE;
    }
    if (output.egl_window != nullptr) {
        wl_egl_window_destroy(output.egl_window);
        output.egl_window = nullptr;
    }
    if (output.layer_surface != nullptr) {
        zwlr_layer_surface_v1_destroy(output.layer_surface);
        output.layer_surface = nullptr;
    }
    if (output.surface != nullptr) {
        wl_surface_destroy(output.surface);
        output.surface = nullptr;
    }
    if (output.output != nullptr) {
        wl_output_destroy(output.output);
        output.output = nullptr;
    }
}

void NativeWallpaperRenderer::remove_output(std::uint32_t registry_name) noexcept {
    if (initialized_) (void)make_pbuffer_current();
    const auto found = std::find_if(
        outputs_.begin(),
        outputs_.end(),
        [registry_name](const auto& output) {
            return output->registry_name == registry_name;
        }
    );
    if (found == outputs_.end()) return;
    destroy_output_surface(**found);
    outputs_.erase(found);
}

void NativeWallpaperRenderer::process_stdin_bytes() {
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
        if (count > 0) {
            stdin_buffer_.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            running_ = false;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        running_ = false;
        break;
    }

    std::size_t newline = 0;
    while ((newline = stdin_buffer_.find('\n')) != std::string::npos) {
        std::string command = stdin_buffer_.substr(0, newline);
        stdin_buffer_.erase(0, newline + 1);
        process_command(command);
    }
}

void NativeWallpaperRenderer::process_command(const std::string& command) {
    if (command == "PING") {
        send_ok();
        return;
    }
    if (command == "QUIT") {
        send_ok();
        running_ = false;
        return;
    }
    if (command == "DISCARD") {
        discard_prepared_wallpaper();
        send_ok();
        return;
    }
    if (command == "COMMIT") {
        std::string error;
        if (!commit_prepared_wallpaper(&error)) {
            send_error(error);
            return;
        }
        if (animating_) {
            set_response_pending_ = true;
            return;
        }
        if (wl_display_roundtrip(display_) < 0) {
            send_error("Wayland failed while confirming prepared wallpaper commit");
            running_ = false;
            return;
        }
        send_ok();
        return;
    }

    const auto decode_path = [&](std::string_view prefix) -> std::optional<std::string> {
        if (!command.starts_with(prefix)) return std::nullopt;
        const std::string encoded = command.substr(prefix.size());
        gsize decoded_size = 0;
        guchar* decoded = g_base64_decode(encoded.c_str(), &decoded_size);
        if (decoded == nullptr || decoded_size == 0) {
            if (decoded != nullptr) g_free(decoded);
            return std::nullopt;
        }
        std::string path(
            reinterpret_cast<const char*>(decoded),
            static_cast<std::size_t>(decoded_size)
        );
        g_free(decoded);
        return path;
    };

    constexpr std::string_view prepare_output_prefix = "PREPARE_OUTPUT ";
    if (command.starts_with(prepare_output_prefix)) {
        const std::string payload = command.substr(prepare_output_prefix.size());
        const std::size_t separator = payload.find(' ');
        if (separator == std::string::npos) {
            send_error("invalid output-targeted wallpaper transaction");
            return;
        }
        const auto decode_token = [](std::string_view encoded)
            -> std::optional<std::string> {
            gsize decoded_size = 0;
            guchar* decoded = g_base64_decode(
                std::string(encoded).c_str(), &decoded_size
            );
            if (decoded == nullptr || decoded_size == 0) {
                if (decoded != nullptr) g_free(decoded);
                return std::nullopt;
            }
            std::string value(
                reinterpret_cast<const char*>(decoded),
                static_cast<std::size_t>(decoded_size)
            );
            g_free(decoded);
            return value;
        };

        const auto output_name = decode_token(
            std::string_view(payload).substr(0, separator)
        );
        const auto path = decode_token(
            std::string_view(payload).substr(separator + 1)
        );
        if (!output_name || !path) {
            send_error("invalid encoded output-targeted wallpaper transaction");
            return;
        }
        std::string error;
        if (!prepare_wallpaper_for_output(*path, *output_name, &error)) {
            send_error(error);
            return;
        }
        std::cout << "PREPARED\n" << std::flush;
        return;
    }

    constexpr std::string_view prepare_prefix = "PREPARE ";
    if (command.starts_with(prepare_prefix)) {
        const auto path = decode_path(prepare_prefix);
        if (!path) {
            send_error("invalid encoded wallpaper path");
            return;
        }
        std::string error;
        if (!prepare_wallpaper(*path, &error)) {
            send_error(error);
            return;
        }
        std::cout << "PREPARED\n" << std::flush;
        return;
    }

    constexpr std::string_view set_prefix = "SET ";
    if (command.starts_with(set_prefix)) {
        const auto path = decode_path(set_prefix);
        if (!path) {
            send_error("invalid encoded wallpaper path");
            return;
        }
        std::string error;
        if (!set_wallpaper(*path, &error)) {
            send_error(error);
            return;
        }
        if (animating_) {
            set_response_pending_ = true;
            return;
        }
        send_ok();
        return;
    }

    send_error("unknown native wallpaper command");
}

void NativeWallpaperRenderer::send_ok() const {
    std::cout << "OK\n" << std::flush;
}

void NativeWallpaperRenderer::send_error(const std::string& message) const {
    std::cout << "ERROR " << base64_encode(message) << '\n' << std::flush;
}

bool NativeWallpaperRenderer::make_pbuffer_current(std::string* error_message) {
    if (egl_display_ == EGL_NO_DISPLAY || egl_context_ == EGL_NO_CONTEXT ||
        pbuffer_surface_ == EGL_NO_SURFACE) {
        set_error(error_message, "EGL pbuffer context is unavailable");
        return false;
    }

    if (eglMakeCurrent(
            egl_display_,
            pbuffer_surface_,
            pbuffer_surface_,
            egl_context_
        ) != EGL_TRUE) {
        set_error(error_message, egl_error_message("eglMakeCurrent"));
        return false;
    }
    return true;
}

std::string NativeWallpaperRenderer::egl_error_message(const char* operation) {
    return std::string(operation) + " failed with EGL error 0x" +
           [] {
               constexpr char digits[] = "0123456789abcdef";
               const unsigned value = static_cast<unsigned>(eglGetError());
               std::string result(4, '0');
               for (int index = 3; index >= 0; --index) {
                   result[static_cast<std::size_t>(index)] = digits[value >> ((3 - index) * 4) & 0xF];
               }
               return result;
           }();
}

void NativeWallpaperRenderer::cleanup() noexcept {
    if (egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT &&
        pbuffer_surface_ != EGL_NO_SURFACE) {
        (void)eglMakeCurrent(
            egl_display_,
            pbuffer_surface_,
            pbuffer_surface_,
            egl_context_
        );
        destroy_texture(prepared_texture_);
        destroy_texture(next_texture_);
        destroy_texture(current_texture_);
        if (program_ != 0) {
            glDeleteProgram(program_);
            program_ = 0;
        }
    }

    for (const auto& output : outputs_) {
        destroy_output_surface(*output);
    }
    outputs_.clear();

    if (pbuffer_surface_ != EGL_NO_SURFACE && egl_display_ != EGL_NO_DISPLAY) {
        eglDestroySurface(egl_display_, pbuffer_surface_);
        pbuffer_surface_ = EGL_NO_SURFACE;
    }
    if (egl_context_ != EGL_NO_CONTEXT && egl_display_ != EGL_NO_DISPLAY) {
        eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }

    if (layer_shell_ != nullptr) {
        zwlr_layer_shell_v1_destroy(layer_shell_);
        layer_shell_ = nullptr;
    }
    if (compositor_ != nullptr) {
        wl_compositor_destroy(compositor_);
        compositor_ = nullptr;
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_ != nullptr) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }

    initialized_ = false;
    running_ = false;
    set_response_pending_ = false;
}

} // namespace realmheart::wallpaper_native
