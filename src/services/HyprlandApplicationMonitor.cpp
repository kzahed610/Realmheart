#include "services/HyprlandApplicationMonitor.hpp"

#include "services/HyprlandEventMonitor.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace realmheart::services {
namespace {

using namespace std::chrono_literals;

bool connect_socket(const std::filesystem::path& path, int& socket_fd) {
    const std::string encoded = path.string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (encoded.empty() || encoded.size() >= sizeof(address.sun_path)) return false;
    std::memcpy(address.sun_path, encoded.c_str(), encoded.size() + 1);

    socket_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return false;
    if (::connect(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        return true;
    }
    ::close(socket_fd);
    socket_fd = -1;
    return false;
}

std::string_view trim_view(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

} // namespace

std::optional<HyprlandApplicationEvent>
parse_hyprland_application_event(std::string_view line) {
    constexpr std::string_view active_prefix = "activewindow>>";
    constexpr std::string_view open_prefix = "openwindow>>";
    constexpr std::array<std::string_view, 4> context_prefixes{
        "activewindowv2>>",
        "closewindow>>",
        "windowtitle>>",
        "windowtitlev2>>",
    };

    if (line.starts_with(active_prefix)) {
        const std::string_view payload = line.substr(active_prefix.size());
        const std::size_t comma = payload.find(',');
        const std::string_view app = trim_view(payload.substr(0, comma));
        if (app.empty()) return std::nullopt;
        return HyprlandApplicationEvent{
            HyprlandApplicationEventKind::Focused,
            std::string(app),
        };
    }

    if (line.starts_with(open_prefix)) {
        // openwindow>>ADDRESS,WORKSPACE,CLASS,TITLE
        const std::string_view payload = line.substr(open_prefix.size());
        const std::size_t first = payload.find(',');
        if (first == std::string_view::npos) return std::nullopt;
        const std::size_t second = payload.find(',', first + 1);
        if (second == std::string_view::npos) return std::nullopt;
        const std::size_t third = payload.find(',', second + 1);
        const std::string_view app = trim_view(payload.substr(
            second + 1,
            third == std::string_view::npos
                ? payload.size() - second - 1
                : third - second - 1
        ));
        if (app.empty()) return std::nullopt;
        return HyprlandApplicationEvent{
            HyprlandApplicationEventKind::Opened,
            std::string(app),
        };
    }

    for (const std::string_view prefix : context_prefixes) {
        if (!line.starts_with(prefix)) continue;
        const std::string_view payload = trim_view(line.substr(prefix.size()));
        return HyprlandApplicationEvent{
            HyprlandApplicationEventKind::ContextChanged,
            std::string(payload),
        };
    }

    return std::nullopt;
}

HyprlandApplicationMonitor::HyprlandApplicationMonitor(EventCallback callback)
    : callback_(std::move(callback)) {}

HyprlandApplicationMonitor::~HyprlandApplicationMonitor() {
    stop();
}

void HyprlandApplicationMonitor::start() {
    if (worker_.joinable() || !callback_) return;

    // Avoid creating a retry thread in tests, TTY sessions, or non-Hyprland
    // environments. Inside a Hyprland session the socket is normally already
    // available, and run() still handles compositor restarts gracefully.
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature == nullptr || *signature == '\0') return;

    worker_ = std::jthread([this](std::stop_token stop_token) { run(stop_token); });
}

void HyprlandApplicationMonitor::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
}

void HyprlandApplicationMonitor::run(std::stop_token stop_token) {
    std::string pending;
    pending.reserve(4096);

    while (!stop_token.stop_requested()) {
        const auto path = HyprlandEventMonitor::event_socket_path();
        int socket_fd = -1;
        if (!path || !connect_socket(*path, socket_fd)) {
            for (int attempt = 0; attempt < 10 && !stop_token.stop_requested(); ++attempt) {
                std::this_thread::sleep_for(100ms);
            }
            continue;
        }

        pending.clear();
        std::array<char, 4096> buffer{};
        while (!stop_token.stop_requested()) {
            pollfd descriptor{socket_fd, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0};
            const int ready = ::poll(&descriptor, 1, 500);
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ready == 0) continue;
            if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) break;

            const ssize_t bytes = ::read(socket_fd, buffer.data(), buffer.size());
            if (bytes <= 0) break;
            pending.append(buffer.data(), static_cast<std::size_t>(bytes));

            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (const auto event = parse_hyprland_application_event(line);
                    event.has_value() && callback_) {
                    callback_(*event);
                }
            }
            if (pending.size() > 64 * 1024) pending.clear();
        }
        ::close(socket_fd);
    }
}

} // namespace realmheart::services
