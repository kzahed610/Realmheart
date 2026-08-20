#pragma once

#include "services/LockSessionProvider.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <cstdint>

namespace realmheart::services {

// LockRendererProcess spawns and manages the realmheart-lockscreen-renderer
// subprocess. It follows the PowerMenuProcess pattern: Unix socket for
// control commands, sibling binary resolution via /proc/self/exe.
//
// The renderer owns the ext-session-lock-v1 protocol: it connects to
// Wayland, acquires the session lock, creates lock surfaces, renders,
// and runs PAM authentication. When auth succeeds, it sends "UNLOCK"
// over the control socket. The parent then calls
// ext_session_lock_v1_destroy() to release the compositor lock.
class LockRendererProcess {
public:
    using AuthSuccessCallback = std::function<void()>;
    using VeilCompleteCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    struct RendererConfig {
        std::uint64_t seed = 0;
        std::vector<std::string> future_paths;  // pre-captured workspace screenshots
    };

    LockRendererProcess(
        LockSessionProvider* session_provider,
        AuthSuccessCallback on_auth_success,
        VeilCompleteCallback on_veil_complete,
        ErrorCallback on_error
    );
    ~LockRendererProcess();

    LockRendererProcess(const LockRendererProcess&) = delete;
    LockRendererProcess& operator=(const LockRendererProcess&) = delete;

    // Spawn the renderer subprocess. Returns true if the process started
    // successfully and sent the READY handshake within timeout.
    [[nodiscard]] bool start(const RendererConfig& config);

    // Send a command to the renderer over the Unix socket.
    void send_command(const std::string& command);

    // Terminate the renderer subprocess.
    void stop();

    // Check if the renderer process is currently running.
    bool is_running() const { return pid_ > 0; }

    // Get the renderer's PID for monitoring.
    int pid() const { return pid_; }

private:
    // Resolve the renderer binary path (sibling of the running executable).
    std::string resolve_renderer_path() const;

    void on_renderer_ready();
    void on_renderer_auth_success();
    void on_renderer_veil_complete();
    void on_renderer_error(const std::string& message);
    void on_renderer_exited();

    LockSessionProvider* session_provider_ = nullptr;
    AuthSuccessCallback on_auth_success_;
    VeilCompleteCallback on_veil_complete_;
    ErrorCallback on_error_;

    RendererConfig config_;

    int pid_ = 0;
    int socket_fd_ = -1;
    std::vector<std::string> pending_commands_;
    bool renderer_ready_ = false;
};

} // namespace realmheart::services
