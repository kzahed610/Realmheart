#pragma once

#include "eventd/FallbackNotifier.hpp"
#include "eventd/RateLimiter.hpp"
#include "events/EventPersistence.hpp"
#include "events/EventStore.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace realmheart::eventd {

class EventDaemonServer {
public:
    EventDaemonServer() = default;
    ~EventDaemonServer();
    EventDaemonServer(const EventDaemonServer&) = delete;
    EventDaemonServer& operator=(const EventDaemonServer&) = delete;

    bool start(std::string& error);
    void run();
    void stop();
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

private:
    struct ClientConnection;

    void handle_client(const std::shared_ptr<ClientConnection>& client);
    realmheart::events::Json handle_request(
        const std::shared_ptr<ClientConnection>& client,
        const realmheart::events::Json& request,
        std::optional<realmheart::events::Json>& broadcast
    );
    void broadcast(const realmheart::events::Json& message);
    bool prepare_socket(std::string& error);
    void prune_clients();
    [[nodiscard]] std::size_t subscriber_count() const;
    [[nodiscard]] SourceTrust classify_source(const ClientConnection& client, const std::string& source_id) const;
    void observe_source(const ClientConnection& client, const realmheart::events::Source& source, SourceTrust trust);
    void persist_mutation(const std::string& op, const realmheart::events::Event& event);
    void maybe_fallback(const realmheart::events::Event& event);
    void clear_fallback_marker(const realmheart::events::EventKey& key);

    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::string socket_path_;
    realmheart::events::EventStore store_;
    realmheart::events::EventPersistence persistence_;
    std::atomic<bool> database_healthy_{false};
    RateLimiter rate_limiter_;
    FallbackNotifier fallback_notifier_;
    mutable std::mutex clients_mutex_;
    std::vector<std::shared_ptr<ClientConnection>> clients_;
    std::vector<std::thread> client_threads_;
    std::atomic<std::uint64_t> action_sequence_{0};
    std::mutex fallback_mutex_;
    std::unordered_set<realmheart::events::EventKey, realmheart::events::EventKeyHash> fallback_notified_;
};

} // namespace realmheart::eventd
