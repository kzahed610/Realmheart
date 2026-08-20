#include "services/SessionManager.hpp"
#include "services/LockRendererProcess.hpp"
#include "services/LockSessionProvider.hpp"
#include "services/ProphecyLayoutEngine.hpp"
#include "services/ProphecyCaptureService.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace realmheart::services {

SessionManager::SessionManager(std::unique_ptr<ICommandExecutor> executor)
    : executor_(std::move(executor)) {}

bool SessionManager::lock() {
    lock_state_ = LockState::Locking;

    // Collect pre-captured workspace screenshots from the prophecy cache.
    // These were captured by ProphecyCaptureService while workspaces were active.
    auto screenshots = ProphecyCaptureService::list_screenshots();
    std::cerr << "SessionManager: found " << screenshots.size()
              << " cached workspace screenshots\n";

    // Try the Prophecy lock screen renderer first.
    session_provider_ = std::make_unique<LockSessionProvider>();

    // Spawn the renderer process. The renderer handles ext-session-lock-v1,
    // creates lock surfaces, renders, and runs PAM auth.
    renderer_process_ = std::make_unique<LockRendererProcess>(
        session_provider_.get(),
        // On auth success: password was valid.
        [this]() {
            lock_state_ = LockState::Authenticating;
        },
        // On veil complete: the renderer released the lock, update state.
        [this]() {
            lock_state_ = LockState::Unlocking;
            lock_state_ = LockState::Unlocked;
        },
        [](const std::string& error) {
            std::cerr << "LockRendererProcess error: " << error << "\n";
        }
    );

    LockRendererProcess::RendererConfig config;
    config.seed = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(session_provider_.get())
    );
    // Pass stored workspace screenshots as future sources.
    for (const auto& ss : screenshots) {
        config.future_paths.push_back(ss.path.string());
    }

    if (renderer_process_->start(config)) {
        // Renderer started and sent READY — session is now locked.
        session_provider_->mark_locked();
        lock_state_ = LockState::Locked;
        return true;
    }

    // Renderer failed to start — fall back to hyprlock.
    std::cerr << "LockRendererProcess: spawn failed, falling back to hyprlock\n";
    session_provider_.reset();
    renderer_process_.reset();
    lock_state_ = LockState::Unlocked;

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
