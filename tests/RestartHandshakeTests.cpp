#include "core/RestartHandshake.hpp"

#include <cstdlib>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    int sockets[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair should be created");

    using namespace realmheart::core::restart_handshake;
    require(send_message(sockets[0], MessageType::HelperReady),
            "helper-ready message should be sent");
    Message message;
    require(wait_for_message(sockets[1], message, 100),
            "helper-ready message should be received");
    require(message.type == MessageType::HelperReady && message.code == 0,
            "helper-ready message should preserve its type and code");

    require(send_message(sockets[1], MessageType::ReleaseOld),
            "release message should be sent");
    require(wait_for_message(sockets[0], message, 100),
            "release message should be received");
    require(message.type == MessageType::ReleaseOld && message.code == 0,
            "release message should preserve its type and code");

    require(send_message(sockets[1], MessageType::Failure, EACCES),
            "failure message should be sent");
    require(wait_for_message(sockets[0], message, 100),
            "failure message should be received");
    require(message.type == MessageType::Failure && message.code == EACCES,
            "failure message should preserve its errno");

    ::close(sockets[0]);
    ::close(sockets[1]);
    int timeout_socket[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, timeout_socket) == 0,
            "timeout socketpair should be created");
    errno = 0;
    require(!wait_for_message(timeout_socket[0], message, 20),
            "empty handshake should time out");
    require(errno == ETIMEDOUT, "empty handshake should report timeout");
    ::close(timeout_socket[0]);
    ::close(timeout_socket[1]);
    return 0;
}
