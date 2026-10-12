#include "wallpaper-native/NativeWallpaperRenderer.hpp"
#include "wallpaper-native/NativeWallpaperContracts.hpp"
#include "wallpaper-native/WallpaperScheduling.hpp"
#include "wallpaper-native/source/RuntimeWallpaperSource.hpp"
#include "wallpaper-native/source/StaticDecodePlan.hpp"
#include "wallpaper-native/source/StaticImageSource.hpp"
#include "wallpaper-native/source/VideoWallpaperSource.hpp"
#include "wallpaper-native/source/WallpaperMediaProbe.hpp"
#include "wallpaper-native/video/VideoDecodeCoordinator.hpp"
#include "wallpaper-native/video/WallpaperVideoStats.hpp"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <poll.h>
#include <string_view>
#include <utility>
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

// A source slot is only drawable while it publishes a frame. An empty slot and
// a frameless source are both treated as "nothing to show", which is what keeps
// the renderer's decision points readable when sources become dynamic.
bool source_has_frame(const RuntimeSourcePtr& source) noexcept {
    return source != nullptr && source->has_frame();
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

#if REALMHEART_HAVE_LIVE_WALLPAPER
    // The decode wakeup channel must exist before any worker can signal it.
    video_coordinator_ = std::make_unique<VideoDecodeCoordinator>();
    if (!video_coordinator_->initialize(error_message)) {
        video_coordinator_.reset();
        cleanup();
        return false;
    }
#endif

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
    if (std::any_of(outputs_.begin(), outputs_.end(), [](const auto& output) {
            return output == nullptr || !output->configured || output->closed;
        })) {
        set_error(error_message, "native wallpaper output surfaces are not renderable");
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
    if (!shaders_.initialize(error_message)) return false;

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

    if (!discard_prepared_wallpaper(error_message)) return false;

    auto candidate = create_path_source(path, error_message);
    if (candidate == nullptr) return false;
    if (!prepare_source(*candidate, error_message)) return false;

    if (!source_has_frame(current_source_)) {
        clear_output_overrides();
        current_source_ = std::move(candidate);
        if (!make_pbuffer_current(error_message)) return false;
        current_source_->start(std::chrono::steady_clock::now());
        if (!draw_all(error_message)) {
            current_source_.reset();
            set_error(error_message, "native wallpaper failed to render the first frame");
            return false;
        }
        const auto required = required_output_dimensions();
        current_source_->set_output_requirement(required.first, required.second);
        return true;
    }

    if (!begin_transition(std::move(candidate), error_message)) return false;
    if (!draw_all(error_message)) {
        next_source_.reset();
        animating_ = false;
        set_error(error_message, "native wallpaper failed to render the transition frame");
        return false;
    }
    return true;
}

bool NativeWallpaperRenderer::set_wallpaper_bytes(
    std::shared_ptr<const std::string> bytes,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!bytes || bytes->empty() || bytes->size() > kNativeMaxSourceBytes) {
        set_error(error_message, "owned wallpaper bytes exceed the decode budget or are empty");
        return false;
    }
    if (!initialized_) {
        set_error(error_message, "native wallpaper renderer is not initialized");
        return false;
    }
    if (!discard_prepared_wallpaper(error_message)) return false;

    auto candidate = create_bytes_source(std::move(bytes), error_message);
    if (candidate == nullptr) return false;
    if (!prepare_source(*candidate, error_message)) return false;
    if (!source_has_frame(current_source_)) {
        clear_output_overrides();
        current_source_ = std::move(candidate);
        if (!make_pbuffer_current(error_message)) return false;
        current_source_->start(std::chrono::steady_clock::now());
        if (!draw_all(error_message)) {
            current_source_.reset();
            set_error(error_message, "native wallpaper failed to render the first frame");
            return false;
        }
        const auto required = required_output_dimensions();
        current_source_->set_output_requirement(required.first, required.second);
        return true;
    }

    if (!begin_transition(std::move(candidate), error_message)) return false;
    if (!draw_all(error_message)) {
        next_source_.reset();
        animating_ = false;
        set_error(error_message, "native wallpaper failed to render the transition frame");
        return false;
    }
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
    if (!discard_prepared_wallpaper(error_message)) return false;
    auto candidate = create_path_source(path, error_message);
    if (candidate == nullptr) return false;
    if (!prepare_source(*candidate, error_message)) return false;
    prepared_source_ = std::move(candidate);
    prepared_output_name_.clear();
    return true;
}

bool NativeWallpaperRenderer::prepare_wallpaper_bytes(
    std::shared_ptr<const std::string> bytes,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!bytes || bytes->empty() || bytes->size() > kNativeMaxSourceBytes) {
        set_error(error_message, "owned wallpaper bytes exceed the decode budget or are empty");
        return false;
    }
    if (!initialized_) {
        set_error(error_message, "native wallpaper renderer is not initialized");
        return false;
    }
    if (animating_) {
        set_error(error_message, "native wallpaper renderer is still transitioning");
        return false;
    }
    if (!discard_prepared_wallpaper(error_message)) return false;
    auto candidate = create_bytes_source(std::move(bytes), error_message);
    if (candidate == nullptr) return false;
    if (!prepare_source(*candidate, error_message)) return false;
    prepared_source_ = std::move(candidate);
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

    if (!discard_prepared_wallpaper(error_message)) return false;
    auto candidate = create_path_source(path, error_message);
    if (candidate == nullptr) return false;
    if (!prepare_source(*candidate, error_message)) return false;
    prepared_source_ = std::move(candidate);
    prepared_output_name_ = output_name;
    return true;
}

bool NativeWallpaperRenderer::prepare_wallpaper_for_output_bytes(
    std::shared_ptr<const std::string> bytes,
    const std::string& output_name,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!bytes || bytes->empty() || bytes->size() > kNativeMaxSourceBytes) {
        set_error(error_message, "owned wallpaper bytes exceed the decode budget or are empty");
        return false;
    }
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

    if (!discard_prepared_wallpaper(error_message)) return false;
    auto candidate = create_bytes_source(std::move(bytes), error_message);
    if (candidate == nullptr) return false;
    if (!prepare_source(*candidate, error_message)) return false;
    prepared_source_ = std::move(candidate);
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
    if (prepared_source_ == nullptr || !prepared_source_->committable()) {
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
            if (!discard_prepared_wallpaper(error_message)) return false;
            return false;
        }
        if (!make_pbuffer_current(error_message)) return false;

        RuntimeSourcePtr previous = std::move(output->override_source);
        output->override_source = std::move(prepared_source_);
        // A video source is only drawable once it has started: prepare() leaves
        // the first frame in the queue without uploading it. Starting it here
        // is what makes a per-output live override visible at all, and
        // playing() keeps a re-apply from restarting one that already runs.
        if (!output->override_source->playing()) {
            output->override_source->start(std::chrono::steady_clock::now());
        }
        if (!output->configured || output->closed ||
            !draw_output(*output, 1.0F, error_message)) {
            // Assigning over the failed candidate releases it, which restores
            // the previous override exactly as the texture version did.
            output->override_source = std::move(previous);
            set_error(error_message, "native wallpaper failed to render the output frame");
            return false;
        }
        previous.reset();
        const auto required = required_output_dimensions(output->name);
        output->override_source->set_output_requirement(
            required.first,
            required.second
        );
        prepared_output_name_.clear();
        return true;
    }

    if (!source_has_frame(current_source_)) {
        clear_output_overrides();
        current_source_ = std::move(prepared_source_);
        if (!make_pbuffer_current(error_message)) return false;
        current_source_->start(std::chrono::steady_clock::now());
        if (!draw_all(error_message)) {
            current_source_.reset();
            set_error(error_message, "native wallpaper failed to render the prepared frame");
            return false;
        }
        const auto required = required_output_dimensions();
        current_source_->set_output_requirement(required.first, required.second);
        return true;
    }

    if (!begin_transition(std::move(prepared_source_), error_message)) return false;
    if (!draw_all(error_message)) {
        next_source_.reset();
        animating_ = false;
        set_error(error_message, "native wallpaper failed to render the prepared transition");
        return false;
    }
    return true;
}

bool NativeWallpaperRenderer::discard_prepared_wallpaper(
    std::string* error_message
) noexcept {
    prepared_output_name_.clear();
    if (prepared_source_ == nullptr || !prepared_source_->committable()) return true;
    // An uninitialized renderer keeps its prepared slot for cleanup(), which
    // releases it with a current context; releasing it here could run the GL
    // deletion without one.
    if (!initialized_) return true;
    if (!make_pbuffer_current(error_message)) {
        mark_fatal(error_message != nullptr ? *error_message : "");
        return false;
    }
    prepared_source_.reset();
    return true;
}

int NativeWallpaperRenderer::run_stdio() {
    if (!initialized_) return 1;
    running_ = true;

    while (running_) {
        if (wl_display_dispatch_pending(display_) < 0) return 1;
        process_closed_outputs();
        if (wl_display_flush(display_) < 0 && errno != EAGAIN) return 1;

        const int coordinator_fd = video_coordinator_ != nullptr &&
                                           video_coordinator_->ready()
                                       ? video_coordinator_->wake_fd()
                                       : -1;
        pollfd descriptors[3] = {
            {wl_display_get_fd(display_), POLLIN, 0},
            {STDIN_FILENO, POLLIN | POLLHUP, 0},
            {coordinator_fd, POLLIN, 0},
        };

        // Deadline-driven: a stable static wallpaper with no transition and no
        // timed source blocks indefinitely (poll timeout -1). A video source
        // contributes its own presentation deadline, and the earliest of the
        // two wins.
        const auto source_deadline = earliest_source_deadline();
        const int timeout = wallpaper_poll_timeout_ms(
            animating_,
            source_deadline,
            std::chrono::steady_clock::now()
        );
        const int result = poll(descriptors, 3, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            return 1;
        }

        if ((descriptors[0].revents & POLLIN) != 0) {
            if (wl_display_dispatch(display_) < 0) return 1;
            process_closed_outputs();
        }

        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
            process_stdin_bytes();
        }

        // A decode worker signals the shared channel when it publishes a frame,
        // loops, or fails. A presentation deadline that expires is the other way
        // a video source needs attention.
        bool video_dirty = false;
        if ((descriptors[2].revents & POLLIN) != 0) {
            video_coordinator_->acknowledge();
            video_dirty = true;
        } else if (result == 0 && source_deadline.has_value() &&
                   std::chrono::steady_clock::now() >= *source_deadline) {
            video_dirty = true;
        }

        if (video_dirty) {
            std::string error;
            bool redraw = false;
            if (!tick_sources(&redraw, &error)) {
                mark_fatal(error);
                return 1;
            }
            if (redraw && !animating_ && !draw_all(&error)) {
                if (set_response_pending_) {
                    set_response_pending_ = false;
                    send_error(error);
                }
                continue;
            }
        }

        if (animating_) advance_animation();
    }

    if (!fatal_error_.empty()) {
        std::cerr << "Native wallpaper renderer stopped: " << fatal_error_ << '\n';
        return 1;
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
            std::string error;
            if (!surface->owner->draw_all(&error) ||
                !surface->owner->redecode_for_topology(&error)) {
                surface->owner->mark_fatal(error);
            }
        } else {
            std::cerr << "Unable to apply output scale to wallpaper: "
                      << error << '\n';
            surface->owner->mark_fatal(error);
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
        output->owner->mark_fatal(error);
        return;
    }

    output->configured = true;
    if (source_has_frame(output->owner->current_source_)) {
        if (!output->owner->draw_output(
                *output,
                output->owner->animating_ ? 0.0F : 1.0F,
                &error
            ) || !output->owner->redecode_for_topology(&error)) {
            output->owner->mark_fatal(error);
        }
    }
}

void NativeWallpaperRenderer::layer_surface_closed(
    void* data,
    zwlr_layer_surface_v1* /*layer_surface*/
) {
    auto* output = static_cast<OutputSurface*>(data);
    output->closed = true;
    if (output->owner == nullptr) return;
    const auto found = std::find(
        output->owner->closed_output_registry_names_.begin(),
        output->owner->closed_output_registry_names_.end(),
        output->registry_name
    );
    if (found == output->owner->closed_output_registry_names_.end()) {
        output->owner->closed_output_registry_names_.push_back(
            output->registry_name
        );
    }
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

bool NativeWallpaperRenderer::prepare_source(
    RuntimeWallpaperSource& source,
    std::string* error_message
) {
    // A source's decode and its GL texture destruction both need a current
    // context. Making the pbuffer current here covers every caller: prepare,
    // transaction discard, and the release of a replaced slot.
    if (!make_pbuffer_current(error_message)) return false;
    return source.prepare(error_message);
}

RuntimeSourcePtr NativeWallpaperRenderer::create_path_source(
    const std::string& path,
    std::string* error_message
) {
    const WallpaperMediaProbe probe = probe_wallpaper_media(path);
    if (probe.type == WallpaperMediaType::Unsupported) {
        set_error(error_message, probe.reason);
        return nullptr;
    }

    // One source per wallpaper identity: the same video as the global wallpaper
    // and as an output override shares its decoder, queue, and presentation
    // clock (renderer docs section 11). The factory only runs on a miss, so a
    // second output with the same path never starts a second decode.
    return source_registry_.acquire(
        path,
        [this, &path, &probe]() -> RuntimeSourcePtr {
            switch (probe.type) {
                case WallpaperMediaType::StaticImage:
                    return make_static_image_source(path);
                case WallpaperMediaType::Video:
#if REALMHEART_HAVE_LIVE_WALLPAPER
                    return make_video_wallpaper_source(
                        path,
                        probe.detail,
                        video_coordinator_.get()
                    );
#else
                    return nullptr;
#endif
                case WallpaperMediaType::Unsupported:
                    break;
            }
            return nullptr;
        },
        error_message
    );
}

bool NativeWallpaperRenderer::tick_sources(
    bool* redraw_required,
    std::string* error_message
) {
    *redraw_required = false;
    if (error_message != nullptr) error_message->clear();
    const auto now = std::chrono::steady_clock::now();

    const auto advance = [&](const RuntimeSourcePtr& source) -> bool {
        if (source == nullptr) return true;
        const SourceTickResult result = source->tick(now);
        if (result.failed) {
            set_error(error_message, result.error);
            return false;
        }
        if (result.redraw_required) *redraw_required = true;
        return true;
    };

    if (!advance(current_source_)) return false;
    for (const auto& output : outputs_) {
        if (output != nullptr && !advance(output->override_source)) return false;
    }
    return true;
}

WallpaperVideoStats NativeWallpaperRenderer::video_stats() const {
    WallpaperVideoStats stats;
    const auto consider = [&stats](const RuntimeSourcePtr& source) {
        if (source == nullptr) return;
        source->collect_stats(stats);
    };

    consider(current_source_);
    for (const auto& output : outputs_) {
        if (output != nullptr) consider(output->override_source);
    }
    if (video_coordinator_ != nullptr) {
        stats.decode_wakeups = video_coordinator_->wakeups();
    }
    return stats;
}

std::optional<std::chrono::steady_clock::time_point>
NativeWallpaperRenderer::earliest_source_deadline() const {
    std::optional<std::chrono::steady_clock::time_point> earliest;
    const auto consider = [&earliest](
                              const RuntimeSourcePtr& source
                          ) {
        if (source == nullptr || source->kind() != WallpaperMediaKind::Video) {
            return;
        }
        const auto deadline = source->next_deadline();
        if (!deadline.has_value()) return;
        if (!earliest.has_value() || *deadline < *earliest) {
            earliest = deadline;
        }
    };

    consider(current_source_);
    for (const auto& output : outputs_) {
        if (output != nullptr) consider(output->override_source);
    }
    return earliest;
}

bool NativeWallpaperRenderer::begin_transition(
    RuntimeSourcePtr incoming,
    std::string* error_message
) {
    // Re-applying the wallpaper that is already current must not restart it.
    // The source registry hands back the very same object for the same path,
    // so this is the same live source: stopping it would tear down the decode
    // worker it is running on, and starting it again would crossfade the source
    // into itself. (BUG-1: the re-apply path used to free the FFmpeg contexts
    // the worker was still decoding with, and the renderer died.)
    if (incoming != nullptr && incoming == current_source_) {
        // A transition to a different wallpaper that was still in flight is
        // cancelled: this wallpaper is the authoritative one now.
        next_source_.reset();
        animating_ = false;
        active_transition_duration_ = transition_duration_;
        if (!make_pbuffer_current(error_message)) return false;
        // Cancelling a transition freezes the outgoing source, and a frozen
        // video would stay frozen on screen forever, so re-anchor it.
        if (!current_source_->playing()) {
            current_source_->start(std::chrono::steady_clock::now());
        }
        if (!draw_all(error_message)) return false;
        return true;
    }

    // The outgoing source stops decoding new frames but keeps its last frame,
    // which is exactly the crossfade source (renderer docs section 9). Releasing
    // it happens when the transition completes.
    if (current_source_ != nullptr) current_source_->stop();
    if (!make_pbuffer_current(error_message)) return false;

    next_source_ = std::move(incoming);
    next_source_->set_output_requirement(0, 0);
    // Establishes playback time zero and presents a first frame, so a committed
    // video is drawable immediately rather than on the next worker wakeup.
    next_source_->start(std::chrono::steady_clock::now());

    active_transition_duration_ = transition_duration_;
    animation_started_ = std::chrono::steady_clock::now();
    animating_ = true;
    return true;
}

RuntimeSourcePtr NativeWallpaperRenderer::create_bytes_source(
    std::shared_ptr<const std::string> bytes,
    std::string* error_message
) {    // Owned payloads stay static-only. A video payload is rejected here with
    // the real reason instead of surfacing as a decode failure.
    if (probe_wallpaper_media_bytes(*bytes).type == WallpaperMediaType::Video) {
        set_error(
            error_message,
            "live wallpaper video currently requires a local file path; "
            "owned video payloads are unsupported"
        );
        return nullptr;
    }
    return make_static_image_source(std::move(bytes));
}

bool NativeWallpaperRenderer::draw_all(std::string* error_message) {
    if (!source_has_frame(current_source_) && !source_has_frame(next_source_)) {
        return true;
    }

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

    bool rendered_output = false;
    bool success = true;
    for (const auto& output : outputs_) {
        if (output->configured && !output->closed) {
            rendered_output = true;
            success = draw_output(*output, progress, error_message) && success;
        }
    }
    if (!rendered_output && error_message != nullptr && error_message->empty()) {
        *error_message = "native wallpaper has no renderable output surfaces";
    }
    if (!rendered_output || !success) {
        mark_fatal(
            error_message != nullptr ? *error_message
                                     : "native wallpaper frame submission failed"
        );
        return false;
    }
    return true;
}

bool NativeWallpaperRenderer::draw_output(
    OutputSurface& output,
    float progress,
    std::string* error_message
) {
    // A per-output override wins over the global wallpaper, exactly as the
    // per-output texture used to; an output without an override draws the
    // global source. Frames are borrowed views, never copied.
    const RenderFrameView override_frame = output.override_source != nullptr
        ? output.override_source->current_frame()
        : RenderFrameView{};
    const RenderFrameView global_frame = source_has_frame(current_source_)
        ? current_source_->current_frame()
        : RenderFrameView{};
    const RenderFrameView current =
        override_frame.valid() ? override_frame : global_frame;
    if (output.egl_surface == EGL_NO_SURFACE || !current.valid()) {
        set_error(error_message, "native wallpaper output is not renderable");
        return false;
    }

    if (eglMakeCurrent(
            egl_display_,
            output.egl_surface,
            output.egl_surface,
            egl_context_
            ) != EGL_TRUE) {
            set_error(error_message, egl_error_message("eglMakeCurrent"));
            return false;
    }

    const int pixel_width = std::max(output.logical_width * output.scale, 1);
    const int pixel_height = std::max(output.logical_height * output.scale, 1);
    glViewport(0, 0, pixel_width, pixel_height);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    // A transition blends the incoming frame over the current one. With no
    // incoming frame the shader still samples two units, so both point at the
    // current frame and progress is forced to 1.0, leaving it untouched.
    const bool has_incoming = source_has_frame(next_source_);
    const RenderFrameView incoming =
        has_incoming ? next_source_->current_frame() : current;
    const auto current_uv = cover_uv(
        current.width(),
        current.height(),
        pixel_width,
        pixel_height
    );
    const auto next_uv = cover_uv(
        incoming.width(),
        incoming.height(),
        pixel_width,
        pixel_height
    );

    shaders_.draw(
        current,
        has_incoming ? incoming : current,
        current_uv,
        next_uv,
        progress
    );

    const GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        set_error(error_message, "OpenGL failed while drawing the wallpaper frame");
        return false;
    }
    if (eglSwapBuffers(egl_display_, output.egl_surface) != EGL_TRUE) {
        set_error(error_message, egl_error_message("eglSwapBuffers"));
        return false;
    }
    return true;
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
    // Releasing a slot releases its GL texture, so callers guarantee a current
    // context (preparing a source, or making the pbuffer current) first.
    for (const auto& output : outputs_) {
        if (output != nullptr) {
            output->override_source.reset();
        }
    }
}

void NativeWallpaperRenderer::advance_animation() {
    const auto elapsed = std::chrono::steady_clock::now() - animation_started_;
    const bool complete = elapsed >= active_transition_duration_;
    std::string error;
    if (!draw_all(&error)) {
        if (set_response_pending_) {
            set_response_pending_ = false;
            send_error(error);
        }
        return;
    }

    if (!complete) return;

    if (!make_pbuffer_current(&error)) {
        if (set_response_pending_) {
            set_response_pending_ = false;
            send_error(error);
        }
        mark_fatal(error);
        return;
    }
    clear_output_overrides();
    // The promoted source keeps the transition's fresh frame; the outgoing one
    // is released by the assignment. The new current starts with no recorded
    // output requirement, exactly as the texture slots did, so the next
    // topology change re-checks coverage.
    current_source_ = std::move(next_source_);
    if (current_source_ != nullptr) {
        current_source_->set_output_requirement(0, 0);
    }
    animating_ = false;
    active_transition_duration_ = transition_duration_;
    if (topology_redecode_pending_) {
        // A topology change waited out the transition; it records the output
        // requirement itself when it re-decodes.
        if (!redecode_for_topology(&error)) {
            if (set_response_pending_) {
                set_response_pending_ = false;
                send_error(error);
            }
            return;
        }
    } else if (current_source_ != nullptr) {
        const auto required = required_output_dimensions();
        current_source_->set_output_requirement(required.first, required.second);
    }
    if (!draw_all(&error)) {
        if (set_response_pending_) {
            set_response_pending_ = false;
            send_error(error);
        }
        return;
    }

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

void NativeWallpaperRenderer::mark_fatal(const std::string& message) noexcept {
    if (fatal_error_.empty()) {
        fatal_error_ = message.empty()
            ? "native wallpaper renderer entered a non-renderable state"
            : message;
    }
    initialized_ = false;
    running_ = false;
}

std::pair<int, int> NativeWallpaperRenderer::required_output_dimensions(
    std::string_view target_output
) const noexcept {
    int width = 1;
    int height = 1;
    for (const auto& output : outputs_) {
        if (output == nullptr || !output->configured || output->closed) continue;
        if (!target_output.empty() && output->name != target_output) continue;
        width = std::max(width, output->logical_width * output->scale);
        height = std::max(height, output->logical_height * output->scale);
    }
    return {width, height};
}

bool NativeWallpaperRenderer::redecode_for_topology(
    std::string* error_message
) {
    if (animating_) {
        topology_redecode_pending_ = true;
        return true;
    }
    if (!source_has_frame(current_source_)) return true;

    // Callers (layer-surface configure, output scale) already drew the new
    // topology, so this only re-derives a source whose coverage box grew.
    // Video sources keep their encoded geometry and report no work, which is
    // why the extra draw stays conditional.
    const auto required = required_output_dimensions();
    if (current_source_->requires_topology_rederive(
            required.first,
            required.second
        )) {
        if (!make_pbuffer_current(error_message)) {
            mark_fatal(error_message != nullptr ? *error_message : "");
            return false;
        }
        if (!current_source_->adapt_to_topology(
                required.first,
                required.second,
                error_message
            )) {
            mark_fatal(error_message != nullptr ? *error_message : "");
            return false;
        }
        if (!draw_all(error_message)) return false;
    }

    for (const auto& output : outputs_) {
        if (output == nullptr || !source_has_frame(output->override_source) ||
            !output->configured || output->closed) {
            continue;
        }
        const int output_width = std::max(output->logical_width * output->scale, 1);
        const int output_height = std::max(output->logical_height * output->scale, 1);
        if (!output->override_source->requires_topology_rederive(
                output_width,
                output_height
            )) {
            continue;
        }
        if (!make_pbuffer_current(error_message)) {
            mark_fatal(error_message != nullptr ? *error_message : "");
            return false;
        }
        if (!output->override_source->adapt_to_topology(
                output_width,
                output_height,
                error_message
            )) {
            mark_fatal(error_message != nullptr ? *error_message : "");
            return false;
        }
        if (!draw_output(*output, 1.0F, error_message)) {
            mark_fatal(error_message != nullptr ? *error_message : "");
            return false;
        }
    }
    topology_redecode_pending_ = false;
    return true;
}

void NativeWallpaperRenderer::process_closed_outputs() {
    if (closed_output_registry_names_.empty() || !running_) return;

    const auto pending = std::exchange(closed_output_registry_names_, {});
    for (const std::uint32_t registry_name : pending) {
        const auto found = std::find_if(
            outputs_.begin(),
            outputs_.end(),
            [registry_name](const auto& output) {
                return output != nullptr && output->registry_name == registry_name;
            }
        );
        if (found == outputs_.end() || (*found)->output == nullptr) {
            mark_fatal("native wallpaper output closed and is no longer available");
            return;
        }

        OutputSurface& output = **found;
        destroy_layer_surface(output);
        std::string error;
        if (!create_output_surface(output, &error) ||
            wl_display_roundtrip(display_) < 0) {
            if (error.empty()) error = "Wayland failed while recreating a closed wallpaper output";
            mark_fatal(error);
            return;
        }
        if (!redecode_for_topology(&error)) {
            mark_fatal(error);
            return;
        }
    }
}

void NativeWallpaperRenderer::destroy_layer_surface(OutputSurface& output) noexcept {
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
    output.configured = false;
    output.closed = false;
}

void NativeWallpaperRenderer::destroy_output_surface(OutputSurface& output) noexcept {
    // An override can only have been prepared while the renderer was
    // initialized, so releasing it here always happens with a current context
    // (and therefore actually deletes the GL texture).
    if (initialized_) {
        (void)make_pbuffer_current();
        output.override_source.reset();
    }
    destroy_layer_surface(output);
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
    if (initialized_ && outputs_.empty()) {
        mark_fatal("Wayland removed the last wallpaper output");
    }
}

void NativeWallpaperRenderer::process_stdin_bytes() {
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
        if (count > 0) {
            std::vector<NativeWallpaperInput> inputs;
            std::string error;
            if (!stdin_decoder_.feed(
                    std::string_view(buffer.data(), static_cast<std::size_t>(count)),
                    &inputs,
                    &error
                )) {
                send_error(error);
                mark_fatal(error);
                break;
            }
            for (auto& input : inputs) {
                if (input.kind == NativeWallpaperInput::Kind::BinaryFrame) {
                    process_binary_frame(std::move(input.binary_frame));
                } else {
                    process_command(input.legacy_command);
                }
                if (!running_) break;
            }
            continue;
        }
        if (count == 0) {
            std::string error;
            if (!stdin_decoder_.finish(&error)) {
                send_error(error);
                mark_fatal(error);
            }
            running_ = false;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        const std::string error = "native wallpaper renderer failed to read its command stream";
        send_error(error);
        mark_fatal(error);
        break;
    }
}

void NativeWallpaperRenderer::process_binary_frame(NativeBinaryFrame frame) {
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

    auto bytes = std::make_shared<const std::string>(std::move(frame.payload));
    std::string error;
    bool succeeded = false;
    switch (frame.command) {
        case NativeBinaryCommand::Set:
            succeeded = set_wallpaper_bytes(std::move(bytes), &error);
            break;
        case NativeBinaryCommand::Prepare:
            succeeded = prepare_wallpaper_bytes(std::move(bytes), &error);
            break;
        case NativeBinaryCommand::PrepareOutput: {
            const auto output_name = decode_token(frame.encoded_output_token);
            if (!output_name) {
                send_error("invalid encoded output-targeted wallpaper transaction");
                return;
            }
            succeeded = prepare_wallpaper_for_output_bytes(
                std::move(bytes), *output_name, &error
            );
            break;
        }
    }
    if (!succeeded) {
        send_error(error);
        return;
    }
    if (frame.command == NativeBinaryCommand::Set) {
        if (animating_) {
            set_response_pending_ = true;
        } else {
            send_ok();
        }
    } else {
        std::cout << "PREPARED\n" << std::flush;
    }
}

void NativeWallpaperRenderer::process_command(const std::string& command) {
    if (command == "PING") {
        send_ok();
        return;
    }
    if (command == "STATS") {
        // One line of counters for the resource governor and Doctor. Answers
        // with data, never with per-frame logging.
        send_raw(format_wallpaper_video_stats(video_stats()));
        return;
    }
    if (command == "QUIT") {
        send_ok();
        running_ = false;
        return;
    }
    if (command == "DISCARD") {
        std::string error;
        if (!discard_prepared_wallpaper(&error)) {
            send_error(error);
            return;
        }
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

void NativeWallpaperRenderer::send_raw(const std::string& line) const {
    std::cout << line << '\n' << std::flush;
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
    // Sources must be released while the pbuffer is still current: releasing a
    // slot deletes its GL texture. Output overrides are held by OutputSurface
    // and are released by destroy_output_surface() below.
    if (egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT &&
        pbuffer_surface_ != EGL_NO_SURFACE) {
        (void)eglMakeCurrent(
            egl_display_,
            pbuffer_surface_,
            pbuffer_surface_,
            egl_context_
        );
        prepared_source_.reset();
        next_source_.reset();
        current_source_.reset();
        shaders_.release();
    } else {
        // Without a context the sources are still released so their workers
        // stop; only an initialized renderer can hold textures.
        prepared_source_.reset();
        next_source_.reset();
        current_source_.reset();
    }

    for (const auto& output : outputs_) {
        destroy_output_surface(*output);
    }
    outputs_.clear();

    if (video_coordinator_ != nullptr) {
        video_coordinator_->shutdown();
        video_coordinator_.reset();
    }

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
    topology_redecode_pending_ = false;
    // Any source that outlived the EGL teardown above is released here without
    // a GL context, which can only happen when the renderer never initialized.
    prepared_source_.reset();
    next_source_.reset();
    current_source_.reset();
    fatal_error_.clear();
    closed_output_registry_names_.clear();
}

} // namespace realmheart::wallpaper_native
