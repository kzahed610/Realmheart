#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for Wayland types we use indirectly.
struct wl_display;
struct wl_registry;
struct ext_session_lock_v1;
struct ext_session_lock_device_v1;
struct ext_session_lock_surface_v1;

namespace realmheart::services {

// LockSessionProvider connects to the compositor's ext-session-lock-v1
// protocol, acquires the session lock, and creates per-output lock surfaces.
//
// This class is security-critical: it runs inside the main shell process
// and holds the authority to release the session lock. The renderer
// subprocess (realmheart-lockscreen-renderer) has NO authority to unlock —
// it signals unlock intent over the control IPC, and the parent calls
// unlock_session_surface / destroy here.
class LockSessionProvider {
public:
    using UnlockCallback = std::function<void()>;

    struct LockResult {
        bool acquired = false;
        std::string error;
    };

    struct OutputInfo {
        std::uint32_t name = 0;
        int width = 0;
        int height = 0;
        int scale = 1;
        std::string make;
        std::string model;
    };

    LockSessionProvider() = default;
    ~LockSessionProvider();

    LockSessionProvider(const LockSessionProvider&) = delete;
    LockSessionProvider& operator=(const LockSessionProvider&) = delete;

    // Connect to the compositor and attempt to acquire the session lock.
    // Returns success/failure. On success, per-output lock surfaces have
    // been created and are ready for the renderer to draw into.
    [[nodiscard]] LockResult acquire();

    // Release the session lock and disconnect from the compositor.
    void release();

    // Called when the renderer signals successful PAM authentication.
    // The parent calls this to actually unlock the session.
    void unlock_session();

    // Get the list of outputs that have lock surfaces.
    const std::vector<OutputInfo>& outputs() const { return outputs_; }

    // Check if currently holding the session lock.
    bool is_locked() const { return locked_; }

    // Wayland surface handle for a given output index — passed to the
    // renderer so it can render into the correct lock surface.
    std::uintptr_t get_surface_handle(std::size_t output_index) const;

    // File descriptor for the Wayland display (for wl_display_connect_to_fd
    // or for passing to the renderer subprocess).
    int display_fd() const;

    // Render one frame for all output surfaces.
    void render_frame();

    // Register callback for when the renderer requests unlock.
    void set_unlock_callback(UnlockCallback callback) {
        unlock_callback_ = std::move(callback);
    }

private:
    void on_lock_acquired();
    void on_lock_surface_configure(std::size_t output_index, int width, int height);
    void on_lock_surface_destroyed(std::size_t output_index);

    static void registry_global(
        void* data, wl_registry* registry,
        std::uint32_t name, const char* interface, std::uint32_t version
    );
    static void registry_global_remove(void* data, wl_registry* registry, std::uint32_t name);
    static void session_lock_acquired(
        void* data, ext_session_lock_v1* lock
    );
    static void session_lock_surface_configure(
        void* data, ext_session_lock_surface_v1* surface,
        std::uint32_t serial, std::uint32_t width, std::uint32_t height,
        std::uint32_t serial_val
    );
    static void session_lock_surface_destroyed(
        void* data, ext_session_lock_surface_v1* surface
    );

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    ext_session_lock_v1* session_lock_ = nullptr;
    ext_session_lock_device_v1* lock_device_ = nullptr;

    struct OutputSurface {
        OutputInfo info;
        ext_session_lock_surface_v1* surface = nullptr;
        void* wayland_surface = nullptr;  // wl_surface* (opaque to parent)
    };
    std::vector<OutputSurface> output_surfaces_;
    std::vector<OutputInfo> outputs_;

    UnlockCallback unlock_callback_;
    bool locked_ = false;
    bool unlock_requested_ = false;
};

} // namespace realmheart::services
