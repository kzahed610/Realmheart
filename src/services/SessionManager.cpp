#include "services/SessionManager.hpp"
#include "services/LockRendererProcess.hpp"
#include "services/LockSessionProvider.hpp"
#include "services/ProphecyLayoutEngine.hpp"

#include <iostream>

namespace realmheart::services {

SessionManager::SessionManager(std::unique_ptr<ICommandExecutor> executor)
    : executor_(std::move(executor)) {}

bool SessionManager::lock() {
    lock_state_ = LockState::Locking;

    // Try the Prophecy lock screen renderer first.
    session_provider_ = std::make_unique<LockSessionProvider>();
    auto result = session_provider_->acquire();

    if (result.acquired) {
        lock_state_ = LockState::Locked;

        // Spawn the renderer process.
        renderer_process_ = std::make_unique<LockRendererProcess>(
            session_provider_.get(),
            // On auth success: password was valid but session stays locked
            // until the handoff veil completes (security != visual state).
            [this]() {
                lock_state_ = LockState::Authenticating;
            },
            // On veil complete: the resolve animation finished, now unlock.
            [this]() {
                lock_state_ = LockState::Unlocking;
                session_provider_->unlock_session();
                lock_state_ = LockState::Unlocked;
            },
            [](const std::string& error) {
                std::cerr << "LockRendererProcess error: " << error << "\n";
            }
        );

        LockRendererProcess::RendererConfig config;
        config.renderer_path = "realmheart-lockscreen-renderer";
        config.socket_path = renderer_process_->socket_path();
        config.seed = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(session_provider_.get())
        );

        // For Phase 4, use a seed-based layout with 5 futures (full spread).
        // The real workspace cache snapshot will be integrated in Phase 5.
        auto layout = ProphecyLayoutEngine::compute(config.seed, 5);

        if (renderer_process_->start(config)) {
            // Send surface handles to the renderer.
            std::string surface_cmd = "SURFACES";
            for (std::size_t i = 0; i < session_provider_->outputs().size(); ++i) {
                surface_cmd += " " + std::to_string(session_provider_->get_surface_handle(i));
            }
            renderer_process_->send_command(surface_cmd);

            // Send the layout (jitter offsets for each future).
            for (const auto& future_geom : layout.futures) {
                renderer_process_->send_command(
                    "LAYOUT " + std::to_string(future_geom.x) + " " +
                    std::to_string(future_geom.y)
                );
            }

            return true;
        }

        // Renderer failed to start — fall back to hyprlock.
        std::cerr << "LockSessionProvider: renderer spawn failed, falling back to hyprlock\n";
        session_provider_->release();
        lock_state_ = LockState::Unlocked;
        session_provider_.reset();
        renderer_process_.reset();
    } else {
        // Acquire failed — fall back to hyprlock.
        std::cerr << "LockSessionProvider: acquire failed, falling back to hyprlock: "
                  << result.error << "\n";
    }

    // Fallback: hyprlock.
    if (executor_->run_background({"hyprlock"})) {
        lock_state_ = LockState::Locked;
        return true;
    }
    lock_state_ = LockState::Unlocked;
    return false;
}

bool SessionManager::suspend() {
    return executor_->run_background({"systemctl", "suspend"});
}

bool SessionManager::logout() {
    return executor_->run_background({"hyprctl", "dispatch", "exit"});
}

bool SessionManager::reboot() {
    return executor_->run_background({"systemctl", "reboot"});
}

bool SessionManager::power_off() {
    return executor_->run_background({"systemctl", "poweroff"});
}

} // namespace realmheart::services
