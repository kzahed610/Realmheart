#include "services/LockSessionProvider.hpp"

#ifdef REALMHEART_HAS_EXT_SESSION_LOCK
#include <wayland-client.h>
#include "ext-session-lock-v1-client-protocol.h"
#endif

#include <iostream>
#include <cstdint>

namespace realmheart::services {

LockSessionProvider::~LockSessionProvider() {
    release();
}

LockSessionProvider::LockResult LockSessionProvider::acquire() {
    LockResult result;
    result.acquired = false;

#ifdef REALMHEART_HAS_EXT_SESSION_LOCK
    // Connect to the Wayland display.
    display_ = wl_display_connect(nullptr);
    if (!display_) {
        result.error = "Failed to connect to Wayland display";
        return result;
    }

    // In a full implementation, we would:
    // 1. Get the registry via wl_display_get_registry
    // 2. Bind to ext_session_lock_manager_v1
    // 3. Call ext_session_lock_manager_v1_lock to acquire the lock
    // 4. On "acquired" event, create surfaces for each output
    //
    // For Phase 4 (skeleton), we stub the actual protocol interaction
    // and focus on the parent-side orchestration: spawning the renderer,
    // passing surface handles, receiving PAM auth results, and calling
    // unlock_session at the right time.

    result.acquired = true;
    locked_ = true;
#else
    result.error = "ext-session-lock-v1 protocol not available";
#endif

    return result;
}

void LockSessionProvider::release() {
#ifdef REALMHEART_HAS_EXT_SESSION_LOCK
    if (display_) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
#endif
    locked_ = false;
}

void LockSessionProvider::unlock_session() {
    if (!locked_) return;

    // In a real implementation, we'd call:
    //   ext_session_lock_v1_destroy(session_lock_)
    // which releases the compositor's session lock, allowing the
    // user's session to become active again.
    locked_ = false;

    if (unlock_callback_) {
        unlock_callback_();
    }
}

void LockSessionProvider::on_lock_acquired() {
    // Called when the compositor signals the lock is acquired.
    // All output surfaces have been created.
}

void LockSessionProvider::on_lock_surface_configure(
    std::size_t output_index, int width, int height
) {
    if (output_index < output_surfaces_.size()) {
        output_surfaces_[output_index].info.width = width;
        output_surfaces_[output_index].info.height = height;
    }
}

void LockSessionProvider::on_lock_surface_destroyed(std::size_t output_index) {
    if (output_index < output_surfaces_.size()) {
        output_surfaces_[output_index].surface = nullptr;
    }
}

#ifdef REALMHEART_HAS_EXT_SESSION_LOCK
// Wayland callback stubs — these would be wired to the generated
// protocol listeners in a full implementation.
void LockSessionProvider::registry_global(
    void* data, wl_registry* /*registry*/,
    std::uint32_t /*name*/, const char* /*interface*/, std::uint32_t /*version*/
) {
    (void)data;
}

void LockSessionProvider::registry_global_remove(
    void* /*data*/, wl_registry* /*registry*/, std::uint32_t /*name*/
) {}

void LockSessionProvider::session_lock_acquired(
    void* data, ext_session_lock_v1* /*lock*/
) {
    if (data) {
        static_cast<LockSessionProvider*>(data)->on_lock_acquired();
    }
}

void LockSessionProvider::session_lock_surface_configure(
    void* data, ext_session_lock_surface_v1* /*surface*/,
    std::uint32_t /*serial*/, std::uint32_t width, std::uint32_t height,
    std::uint32_t /*serial_val*/
) {
    // The output_index lookup would happen via the surface pointer.
    // For the skeleton, we just call through with a placeholder index.
    if (data) {
        static_cast<LockSessionProvider*>(data)->on_lock_surface_configure(0, width, height);
    }
}

void LockSessionProvider::session_lock_surface_destroyed(
    void* data, ext_session_lock_surface_v1* /*surface*/
) {
    if (data) {
        static_cast<LockSessionProvider*>(data)->on_lock_surface_destroyed(0);
    }
}
#endif

std::uintptr_t LockSessionProvider::get_surface_handle(std::size_t output_index) const {
    if (output_index >= output_surfaces_.size()) return 0;
    return reinterpret_cast<std::uintptr_t>(output_surfaces_[output_index].surface);
}

int LockSessionProvider::display_fd() const {
    if (!display_) return -1;
    // wl_display_get_fd returns a file descriptor for the Wayland connection.
    // In a real implementation we'd call wl_display_get_fd(display_, ...).
    return -1;  // Placeholder
}

void LockSessionProvider::render_frame() {
    // In a full implementation, this would:
    // 1. Call wl_surface_attach on each lock surface
    // 2. Commit the surface
    // 3. Call wl_display_flush
    // For Phase 4, this is a no-op stub.
}

} // namespace realmheart::services
