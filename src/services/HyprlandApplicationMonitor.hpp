#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace realmheart::services {

enum class HyprlandApplicationEventKind {
    Opened,
    Focused,
    ContextChanged,
};

struct HyprlandApplicationEvent {
    HyprlandApplicationEventKind kind = HyprlandApplicationEventKind::Focused;
    std::string app_identity;
};

// Parses only events that reveal an application identity. This deliberately
// ignores title-only, workspace, and address-only events.
[[nodiscard]] std::optional<HyprlandApplicationEvent>
parse_hyprland_application_event(std::string_view line);

// Watches Hyprland's socket2 stream and reports application open/focus events.
// The callback runs on the monitor thread and must not touch GTK directly.
class HyprlandApplicationMonitor {
public:
    using EventCallback = std::function<void(const HyprlandApplicationEvent&)>;

    explicit HyprlandApplicationMonitor(EventCallback callback);
    ~HyprlandApplicationMonitor();

    HyprlandApplicationMonitor(const HyprlandApplicationMonitor&) = delete;
    HyprlandApplicationMonitor& operator=(const HyprlandApplicationMonitor&) = delete;

    void start();
    void stop();

private:
    void run(std::stop_token stop_token);

    EventCallback callback_;
    std::jthread worker_;
};

} // namespace realmheart::services
