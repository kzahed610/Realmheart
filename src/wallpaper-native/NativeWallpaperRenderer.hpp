#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

namespace realmheart::wallpaper_native {

class NativeWallpaperRenderer {
public:
    NativeWallpaperRenderer() = default;
    ~NativeWallpaperRenderer();

    NativeWallpaperRenderer(const NativeWallpaperRenderer&) = delete;
    NativeWallpaperRenderer& operator=(const NativeWallpaperRenderer&) = delete;
    NativeWallpaperRenderer(NativeWallpaperRenderer&&) = delete;
    NativeWallpaperRenderer& operator=(NativeWallpaperRenderer&&) = delete;

    [[nodiscard]] bool initialize(std::string* error_message = nullptr);
    [[nodiscard]] bool set_wallpaper(
        const std::string& path,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool prepare_wallpaper(
        const std::string& path,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool prepare_wallpaper_for_output(
        const std::string& path,
        const std::string& output_name,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool commit_prepared_wallpaper(
        std::string* error_message = nullptr
    );
    void discard_prepared_wallpaper() noexcept;
    int run_stdio();

private:
    struct Texture {
        GLuint id = 0;
        int width = 0;
        int height = 0;
    };

    struct OutputSurface {
        NativeWallpaperRenderer* owner = nullptr;
        std::uint32_t registry_name = 0;
        wl_output* output = nullptr;
        wl_surface* surface = nullptr;
        zwlr_layer_surface_v1* layer_surface = nullptr;
        wl_egl_window* egl_window = nullptr;
        EGLSurface egl_surface = EGL_NO_SURFACE;
        int logical_width = 0;
        int logical_height = 0;
        int scale = 1;
        std::string name;
        Texture override_texture;
        bool configured = false;
        bool closed = false;
    };

public:
    // Wayland callbacks are public only so C listener tables can reference them.
    static void registry_global(
        void* data,
        wl_registry* registry,
        std::uint32_t name,
        const char* interface,
        std::uint32_t version
    );
    static void registry_global_remove(
        void* data,
        wl_registry* registry,
        std::uint32_t name
    );

    static void output_geometry(
        void* data,
        wl_output* output,
        std::int32_t x,
        std::int32_t y,
        std::int32_t physical_width,
        std::int32_t physical_height,
        std::int32_t subpixel,
        const char* make,
        const char* model,
        std::int32_t transform
    );
    static void output_mode(
        void* data,
        wl_output* output,
        std::uint32_t flags,
        std::int32_t width,
        std::int32_t height,
        std::int32_t refresh
    );
    static void output_done(void* data, wl_output* output);
    static void output_scale(void* data, wl_output* output, std::int32_t factor);
    static void output_name(void* data, wl_output* output, const char* name);
    static void output_description(void* data, wl_output* output, const char* description);

    static void layer_surface_configure(
        void* data,
        zwlr_layer_surface_v1* layer_surface,
        std::uint32_t serial,
        std::uint32_t width,
        std::uint32_t height
    );
    static void layer_surface_closed(
        void* data,
        zwlr_layer_surface_v1* layer_surface
    );

private:
    [[nodiscard]] bool initialize_wayland(std::string* error_message);
    [[nodiscard]] bool initialize_egl(std::string* error_message);
    [[nodiscard]] bool initialize_gl(std::string* error_message);
    [[nodiscard]] bool create_output_surface(
        OutputSurface& output,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool configure_egl_surface(
        OutputSurface& output,
        int width,
        int height,
        std::string* error_message = nullptr
    );

    [[nodiscard]] bool upload_texture(
        const std::string& path,
        Texture& texture,
        std::string* error_message,
        std::string_view target_output = {}
    );
    [[nodiscard]] GLuint compile_shader(
        GLenum type,
        const char* source,
        std::string* error_message
    );
    [[nodiscard]] bool link_program(std::string* error_message);

    void draw_all();
    void draw_output(OutputSurface& output, float progress);
    [[nodiscard]] OutputSurface* find_output_by_name(
        std::string_view name
    ) noexcept;
    void clear_output_overrides() noexcept;
    void advance_animation();
    void destroy_texture(Texture& texture) noexcept;
    void destroy_output_surface(OutputSurface& output) noexcept;
    void remove_output(std::uint32_t registry_name) noexcept;
    void process_stdin_bytes();
    void process_command(const std::string& command);
    void send_ok() const;
    void send_error(const std::string& message) const;
    void cleanup() noexcept;

    [[nodiscard]] bool make_pbuffer_current(std::string* error_message = nullptr);
    [[nodiscard]] static std::string egl_error_message(const char* operation);

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    std::vector<std::unique_ptr<OutputSurface>> outputs_;

    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLConfig egl_config_ = nullptr;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface pbuffer_surface_ = EGL_NO_SURFACE;

    GLuint program_ = 0;
    GLint position_attribute_ = -1;
    GLint uv_attribute_ = -1;
    GLint current_sampler_uniform_ = -1;
    GLint next_sampler_uniform_ = -1;
    GLint progress_uniform_ = -1;
    GLint current_uv_uniform_ = -1;
    GLint next_uv_uniform_ = -1;


    Texture current_texture_;
    Texture next_texture_;
    Texture prepared_texture_;
    std::string prepared_output_name_;
    bool animating_ = false;
    std::chrono::steady_clock::time_point animation_started_{};
    std::chrono::milliseconds transition_duration_{350};
    std::chrono::milliseconds active_transition_duration_{350};
    bool initialized_ = false;
    bool running_ = false;
    bool set_response_pending_ = false;
    std::string stdin_buffer_;
};

} // namespace realmheart::wallpaper_native
