#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace realmheart::core::restart_handshake {

inline constexpr int kInheritedFd = 198;
inline constexpr int kTimeoutMilliseconds = 15000;
inline constexpr std::size_t kMessageSize = 5;

enum class MessageType : std::uint8_t {
    HelperReady = 'H',
    ReleaseOld = 'R',
    ShellReady = 'S',
    Failure = 'F',
};

struct Message {
    MessageType type = MessageType::Failure;
    int code = EPROTO;
};

inline bool is_valid_message_type(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(MessageType::HelperReady) ||
           value == static_cast<std::uint8_t>(MessageType::ReleaseOld) ||
           value == static_cast<std::uint8_t>(MessageType::ShellReady) ||
           value == static_cast<std::uint8_t>(MessageType::Failure);
}

inline bool write_all(int fd, const std::uint8_t* data, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::send(
            fd,
            data + offset,
            size - offset,
            MSG_NOSIGNAL
        );
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (errno == 0) errno = EPIPE;
        return false;
    }
    return true;
}

inline bool read_all(int fd, std::uint8_t* data, std::size_t size) noexcept {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t read_count = ::read(fd, data + offset, size - offset);
        if (read_count > 0) {
            offset += static_cast<std::size_t>(read_count);
            continue;
        }
        if (read_count < 0 && errno == EINTR) continue;
        if (read_count == 0) errno = ECONNRESET;
        return false;
    }
    return true;
}

inline bool send_message(int fd, MessageType type, int code = 0) noexcept {
    const auto encoded_code = static_cast<std::uint32_t>(code);
    std::array<std::uint8_t, kMessageSize> packet{
        static_cast<std::uint8_t>(type),
        static_cast<std::uint8_t>((encoded_code >> 24) & 0xffU),
        static_cast<std::uint8_t>((encoded_code >> 16) & 0xffU),
        static_cast<std::uint8_t>((encoded_code >> 8) & 0xffU),
        static_cast<std::uint8_t>(encoded_code & 0xffU),
    };
    return write_all(fd, packet.data(), packet.size());
}

inline bool receive_message(int fd, Message& message) noexcept {
    std::array<std::uint8_t, kMessageSize> packet{};
    if (!read_all(fd, packet.data(), packet.size())) {
        return false;
    }
    if (!is_valid_message_type(packet[0])) {
        errno = EPROTO;
        return false;
    }

    const std::uint32_t encoded_code =
        (static_cast<std::uint32_t>(packet[1]) << 24) |
        (static_cast<std::uint32_t>(packet[2]) << 16) |
        (static_cast<std::uint32_t>(packet[3]) << 8) |
        static_cast<std::uint32_t>(packet[4]);
    if (encoded_code > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        errno = EPROTO;
        return false;
    }

    message.type = static_cast<MessageType>(packet[0]);
    message.code = static_cast<int>(encoded_code);
    return true;
}

inline bool wait_for_message(
    int fd,
    Message& message,
    int timeout_milliseconds
) noexcept {
    struct pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN | POLLHUP;
    const int poll_result = ::poll(&descriptor, 1, timeout_milliseconds);
    if (poll_result == 0) {
        errno = ETIMEDOUT;
        return false;
    }
    if (poll_result < 0 ||
        (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
        if (poll_result > 0) errno = ECONNRESET;
        return false;
    }
    return receive_message(fd, message);
}

} // namespace realmheart::core::restart_handshake
