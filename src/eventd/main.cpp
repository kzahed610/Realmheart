#include "eventd/EventDaemonServer.hpp"
#include "events/EventTransport.hpp"

#include <csignal>
#include <iostream>
#include <pthread.h>
#include <string>
#include <string_view>
#include <thread>

#ifndef REALMHEART_VERSION
#define REALMHEART_VERSION "unknown"
#endif

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "Realmheart Event Daemon " << REALMHEART_VERSION << '\n';
        return 0;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        std::cerr << "realmheart-eventd: unable to block termination signals\n";
        return 1;
    }

    realmheart::eventd::EventDaemonServer server;
    std::string error;
    if (!server.start(error)) {
        std::cerr << "realmheart-eventd: " << error << '\n';
        return 1;
    }

    std::cout << "realmheart-eventd: listening on "
              << realmheart::events::default_socket_path() << '\n';
    std::thread server_thread([&server] { server.run(); });

    int received_signal = 0;
    static_cast<void>(sigwait(&signals, &received_signal));
    server.stop();
    if (server_thread.joinable()) server_thread.join();
    return 0;
}
