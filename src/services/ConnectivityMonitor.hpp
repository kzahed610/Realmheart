#pragma once

#include <gio/gio.h>

#include <functional>
#include <memory>

namespace realmheart::services {

struct ConnectivityMonitorState;

class ConnectivityMonitor {
public:
    using ChangedCallback = std::function<void()>;

    // Passing a connection is intended for isolated tests. Production monitors
    // subscribe to the system bus.
    explicit ConnectivityMonitor(
        ChangedCallback changed,
        GDBusConnection* injected_connection = nullptr
    );
    ~ConnectivityMonitor();

    ConnectivityMonitor(const ConnectivityMonitor&) = delete;
    ConnectivityMonitor& operator=(const ConnectivityMonitor&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool signal_monitor_active() const noexcept;

private:
    std::shared_ptr<ConnectivityMonitorState> state_;
};

} // namespace realmheart::services
