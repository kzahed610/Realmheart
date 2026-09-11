#include "events/EventTransport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace realmheart::events {
namespace {

bool write_all(int fd, const void* data, std::size_t size, std::string& error) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = ::send(fd, bytes + written, size - written, MSG_NOSIGNAL);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        error = std::string("socket write failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool read_all(int fd, void* data, std::size_t size, std::string& error) {
    auto* bytes = static_cast<unsigned char*>(data);
    std::size_t read_count = 0;
    while (read_count < size) {
        const ssize_t count = ::recv(fd, bytes + read_count, size - read_count, 0);
        if (count > 0) {
            read_count += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            error = "socket closed";
            return false;
        }
        if (errno == EINTR) continue;
        error = std::string("socket read failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

std::string default_socket_path() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0') {
        return std::string(runtime) + "/realmheart/eventd.sock";
    }
    return std::string("/tmp/realmheart-") + std::to_string(::getuid()) + "/eventd.sock";
}

bool send_frame(int fd, const Json& payload, std::string& error) {
    const std::string encoded = payload.dump();
    if (encoded.size() > kMaxPayloadBytes) {
        error = "payload exceeds 1 MiB protocol limit";
        return false;
    }
    const std::uint32_t network_size = htonl(static_cast<std::uint32_t>(encoded.size()));
    return write_all(fd, &network_size, sizeof(network_size), error) &&
           write_all(fd, encoded.data(), encoded.size(), error);
}

std::optional<Json> receive_frame(int fd, std::string& error) {
    std::uint32_t network_size = 0;
    if (!read_all(fd, &network_size, sizeof(network_size), error)) return std::nullopt;
    const std::uint32_t payload_size = ntohl(network_size);
    if (payload_size == 0 || payload_size > kMaxPayloadBytes) {
        error = "invalid or oversized frame";
        return std::nullopt;
    }
    std::string encoded(payload_size, '\0');
    if (!read_all(fd, encoded.data(), encoded.size(), error)) return std::nullopt;
    try {
        return Json::parse(encoded);
    } catch (const Json::exception& exception) {
        error = std::string("invalid JSON frame: ") + exception.what();
        return std::nullopt;
    }
}

int connect_event_socket(std::string& error) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        error = std::string("socket() failed: ") + std::strerror(errno);
        return -1;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = default_socket_path();
    if (path.size() >= sizeof(address.sun_path)) {
        error = "event socket path is too long";
        ::close(fd);
        return -1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = std::string("unable to connect to ") + path + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace realmheart::events
