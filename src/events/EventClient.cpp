#include "events/EventClient.hpp"
#include "events/EventTransport.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace realmheart::events {

Json EventClient::request(const Json& request_payload, std::string& error) {
    const int fd = connect_event_socket(error);
    if (fd < 0) return error_response("connection_failed", error);

    if (!send_frame(fd, request_payload, error)) {
        ::close(fd);
        return error_response("send_failed", error);
    }
    auto response = receive_frame(fd, error);
    ::close(fd);
    if (!response) return error_response("receive_failed", error);
    return *response;
}

struct EventSubscriber::State {
    std::atomic<bool> running{false};
    std::atomic<int> active_fd{-1};
    std::thread worker;
    MessageHandler message_handler;
    ConnectionHandler connection_handler;
};

EventSubscriber::~EventSubscriber() { stop(); }

void EventSubscriber::start(MessageHandler message_handler, ConnectionHandler connection_handler) {
    stop();
    state_ = new State;
    state_->message_handler = std::move(message_handler);
    state_->connection_handler = std::move(connection_handler);
    state_->running.store(true);
    State* state = state_;
    state_->worker = std::thread([state] {
        bool previously_connected = false;
        while (state->running.load()) {
            std::string error;
            const int fd = connect_event_socket(error);
            if (fd < 0) {
                if (previously_connected && state->connection_handler) state->connection_handler(false);
                previously_connected = false;
                for (int i = 0; i < 10 && state->running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            state->active_fd.store(fd);
            if (state->connection_handler && !previously_connected) state->connection_handler(true);
            previously_connected = true;

            const Json subscribe{{"protocol", kProtocolVersion}, {"op", "subscribe"}};
            if (!send_frame(fd, subscribe, error)) {
                ::close(fd);
                state->active_fd.store(-1);
                continue;
            }

            while (state->running.load()) {
                auto message = receive_frame(fd, error);
                if (!message) break;
                if (state->message_handler) state->message_handler(*message);
            }
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            state->active_fd.store(-1);
            if (state->running.load() && state->connection_handler) state->connection_handler(false);
            previously_connected = false;
        }
    });
}

void EventSubscriber::stop() {
    if (state_ == nullptr) return;
    state_->running.store(false);
    const int fd = state_->active_fd.exchange(-1);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    if (state_->worker.joinable()) state_->worker.join();
    delete state_;
    state_ = nullptr;
}


struct EventActionListener::State {
    std::atomic<bool> running{false};
    std::atomic<int> active_fd{-1};
    std::thread worker;
    std::string source_id;
    InvocationHandler invocation_handler;
    ConnectionHandler connection_handler;
};

EventActionListener::~EventActionListener() { stop(); }

void EventActionListener::start(
    std::string source_id,
    InvocationHandler invocation_handler,
    ConnectionHandler connection_handler
) {
    stop();
    state_ = new State;
    state_->source_id = std::move(source_id);
    state_->invocation_handler = std::move(invocation_handler);
    state_->connection_handler = std::move(connection_handler);
    state_->running.store(true);
    State* state = state_;
    state_->worker = std::thread([state] {
        bool previously_connected = false;
        while (state->running.load()) {
            std::string error;
            const int fd = connect_event_socket(error);
            if (fd < 0) {
                if (previously_connected && state->connection_handler) state->connection_handler(false);
                previously_connected = false;
                for (int i = 0; i < 10 && state->running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            state->active_fd.store(fd);

            const Json registration{
                {"protocol", kProtocolVersion},
                {"op", "register_source"},
                {"source_id", state->source_id}
            };
            if (!send_frame(fd, registration, error)) {
                ::close(fd);
                state->active_fd.store(-1);
                continue;
            }
            auto response = receive_frame(fd, error);
            if (!response || !response->value("ok", false)) {
                ::shutdown(fd, SHUT_RDWR);
                ::close(fd);
                state->active_fd.store(-1);
                for (int i = 0; i < 10 && state->running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            if (state->connection_handler && !previously_connected) state->connection_handler(true);
            previously_connected = true;
            while (state->running.load()) {
                auto message = receive_frame(fd, error);
                if (!message) break;
                if (message->value("type", "") == "ACTION_INVOKED" && state->invocation_handler) {
                    state->invocation_handler(*message);
                }
            }

            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            state->active_fd.store(-1);
            if (state->running.load() && state->connection_handler) state->connection_handler(false);
            previously_connected = false;
        }
    });
}

void EventActionListener::stop() {
    if (state_ == nullptr) return;
    state_->running.store(false);
    const int fd = state_->active_fd.exchange(-1);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    if (state_->worker.joinable()) state_->worker.join();
    delete state_;
    state_ = nullptr;
}

} // namespace realmheart::events
