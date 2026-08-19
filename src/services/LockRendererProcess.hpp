#pragma once

#include "services/LockSessionProvider.hpp"
#include "services/WorkspaceProphecyCache.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <cstdint>

namespace realmheart::services {

// LockRendererProcess spawns and manages the realmheart-lockscreen-renderer
// subprocess. It follows the PowerMenuProcess pattern: Unix socket for
// commands, stdio for readiness, g_child_watch_add for reaping.
//
// The renderer does PAM authentication and renders into the lock surfaces
// provided by LockSessionProvider. When the renderer signals successful
// auth, the parent calls LockSessionProvider::unlock_session().
class LockRendererProcess {
public:
    using AuthSuccessCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    struct RendererConfig {
        std::string renderer_path;
        std::string socket_path;
        std::string background_asset;
        std::string rinia_asset;
        std::uint64_t seed = 0;
    };

    LockRendererProcess(
        LockSessionProvider* session_provider,
        AuthSuccessCallback on_auth_success,
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

    // Called by the parent's Wayland dispatch loop when data is available
    // on the control socket. Dispatches commands from the renderer.
    void on_socket_readable();

    // Terminate the renderer subprocess.
    void stop();

    // Check if the renderer process is currently running.
    bool is_running() const { return pid_ > 0; }

    // Get the renderer's PID for monitoring.
    int pid() const { return pid_; }

    // Get the control socket path (for passing to the renderer subprocess).
    const std::string& socket_path() const { return socket_path_; }

private:
    void on_renderer_ready();
    void on_renderer_auth_success();
    void on_renderer_error(const std::string& message);
    void on_renderer_exited();

    LockSessionProvider* session_provider_ = nullptr;
    AuthSuccessCallback on_auth_success_;
    ErrorCallback on_error_;

    std::string socket_path_;
    RendererConfig config_;

    int pid_ = 0;
    int socket_fd_ = -1;
    std::vector<std::string> pending_commands_;

    // The workspace cache snapshot is sent to the renderer at startup.
    // The renderer needs it to know what futures to display.
    WorkspaceProphecyCache::Selection prophecy_selection_;
    bool renderer_ready_ = false;
};

} // namespace realmheart::services
