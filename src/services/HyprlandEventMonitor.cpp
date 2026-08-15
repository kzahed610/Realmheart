#include "services/HyprlandEventMonitor.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

} // namespace

HyprlandEventMonitor::HyprlandEventMonitor(ChangedCallback callback)
    : callback_(std::move(callback)) {}

HyprlandEventMonitor::~HyprlandEventMonitor() {
    stop();
}

void HyprlandEventMonitor::start() {
    if (worker_.joinable() || !callback_) return;
    worker_ = std::jthread([this](std::stop_token stop_token) { run(stop_token); });
}

void HyprlandEventMonitor::stop() {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
}

std::optional<std::filesystem::path> HyprlandEventMonitor::event_socket_path() {
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature == nullptr || *signature == '\0') return std::nullopt;

    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && *runtime != '\0') {
        const auto modern = std::filesystem::path(runtime) / "hypr" / signature / ".socket2.sock";
        std::error_code error;
        if (std::filesystem::exists(modern, error) && !error) return modern;
    }

    const auto legacy = std::filesystem::path("/tmp/hypr") / signature / ".socket2.sock";
    std::error_code error;
    if (std::filesystem::exists(legacy, error) && !error) return legacy;
    return std::nullopt;
}

bool HyprlandEventMonitor::is_workspace_event(std::string_view line) {
    constexpr std::string_view prefixes[] = {
        "workspace>>", "workspacev2>>",
        "createworkspace>>", "createworkspacev2>>",
        "destroyworkspace>>", "destroyworkspacev2>>",
        "moveworkspace>>", "moveworkspacev2>>",
        "focusedmon>>",
        "openwindow>>", "closewindow>>",
        "movewindow>>", "movewindowv2>>",
        "windowtitle>>", "windowtitlev2>>",
    };
    for (const auto prefix : prefixes) {
        if (line.starts_with(prefix)) return true;
    }
    return false;
}

void HyprlandEventMonitor::run(std::stop_token stop_token) {
    std::string pending;
    pending.reserve(4096);

    while (!stop_token.stop_requested()) {
        const auto path = event_socket_path();
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
                if (is_workspace_event(line) && callback_) callback_();
            }
            if (pending.size() > 64 * 1024) pending.clear();
        }
        ::close(socket_fd);
    }
}

} // namespace realmheart::services
