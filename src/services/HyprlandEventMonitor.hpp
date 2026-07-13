#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>

namespace realmheart::services {

// Watches Hyprland's event socket and coalesces only events that can change the
// workspace strip. The callback runs on the monitor thread and must not touch
// GTK directly.
class HyprlandEventMonitor {
public:
    using ChangedCallback = std::function<void()>;

    explicit HyprlandEventMonitor(ChangedCallback callback);
    ~HyprlandEventMonitor();

    HyprlandEventMonitor(const HyprlandEventMonitor&) = delete;
    HyprlandEventMonitor& operator=(const HyprlandEventMonitor&) = delete;

    void start();
    void stop();

    [[nodiscard]] static std::optional<std::filesystem::path> event_socket_path();
    [[nodiscard]] static bool is_workspace_event(std::string_view line);

private:
    void run(std::stop_token stop_token);

    ChangedCallback callback_;
    std::jthread worker_;
};

} // namespace realmheart::services
