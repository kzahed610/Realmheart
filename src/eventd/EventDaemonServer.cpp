#include "eventd/EventDaemonServer.hpp"
#include "events/EventTransport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace realmheart::eventd {
using realmheart::events::Event;
using realmheart::events::EventKey;
using realmheart::events::Json;
using realmheart::events::Source;
using realmheart::events::SourceRecord;
using realmheart::events::ValidationResult;

namespace {

std::string peer_executable(pid_t pid) {
    if (pid <= 0) return {};
    const std::string path = "/proc/" + std::to_string(pid) + "/exe";
    std::string result(4096, '\0');
    const ssize_t length = ::readlink(path.c_str(), result.data(), result.size() - 1U);
    if (length <= 0) return {};
    result.resize(static_cast<std::size_t>(length));
    return result;
}

bool starts_with_realmheart(const std::string& value) {
    return value == "realmheart" || value.rfind("realmheart-", 0) == 0 || value.rfind("realmheart.", 0) == 0;
}

Json source_record_to_json(const SourceRecord& source) {
    return {
        {"id", source.id},
        {"display_name", source.display_name},
        {"icon", source.icon},
        {"uid", source.uid},
        {"last_pid", source.last_pid},
        {"executable", source.executable},
        {"trust_class", source.trust_class},
        {"first_seen", source.first_seen},
        {"last_seen", source.last_seen}
    };
}

} // namespace

struct EventDaemonServer::ClientConnection {
    int fd = -1;
    std::atomic<bool> alive{true};
    std::atomic<bool> subscribed{false};
    std::string action_source_id;
    uid_t peer_uid = 0;
    pid_t peer_pid = 0;
    std::string peer_executable;
    std::mutex send_mutex;
};

EventDaemonServer::~EventDaemonServer() { stop(); }

bool EventDaemonServer::prepare_socket(std::string& error) {
    socket_path_ = realmheart::events::default_socket_path();
    const std::filesystem::path path(socket_path_);
    std::error_code fs_error;
    std::filesystem::create_directories(path.parent_path(), fs_error);
    if (fs_error) {
        error = "unable to create runtime directory: " + fs_error.message();
        return false;
    }
    if (::chmod(path.parent_path().c_str(), S_IRWXU) != 0) {
        error = std::string("unable to secure runtime directory: ") + std::strerror(errno);
        return false;
    }

    ::unlink(socket_path_.c_str());
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        error = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(address.sun_path)) {
        error = "event socket path is too long";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1U);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = std::string("bind() failed: ") + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
        error = std::string("unable to secure event socket: ") + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return false;
    }
    if (::listen(listen_fd_, 64) != 0) {
        error = std::string("listen() failed: ") + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return false;
    }
    return true;
}

bool EventDaemonServer::start(std::string& error) {
    if (running_.load()) return true;

    std::string database_error;
    if (persistence_.open(database_error)) {
        database_healthy_.store(true);
        std::string cleanup_error;
        if (!persistence_.cleanup_history(30, 10000U, cleanup_error)) {
            database_healthy_.store(false);
            std::cerr << "[eventd] history cleanup failed: " << cleanup_error << '\n';
        }

        std::string revision_error;
        const std::uint64_t sequence_floor = persistence_.max_revision(revision_error);
        if (!revision_error.empty()) {
            database_healthy_.store(false);
            std::cerr << "[eventd] unable to read persisted revision: " << revision_error << '\n';
        }
        std::string load_error;
        auto restored = persistence_.load_active(load_error);
        if (!load_error.empty()) {
            database_healthy_.store(false);
            std::cerr << "[eventd] unable to restore persistent events: " << load_error << '\n';
        } else {
            store_.restore(std::move(restored), sequence_floor);
        }
    } else {
        std::cerr << "[eventd] SQLite unavailable; continuing in memory-only mode: " << database_error << '\n';
    }

    if (!prepare_socket(error)) return false;
    running_.store(true);
    return true;
}

void EventDaemonServer::run() {
    while (running_.load()) {
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!running_.load() || errno == EBADF || errno == EINVAL) break;
            std::cerr << "[eventd] accept failed: " << std::strerror(errno) << '\n';
            continue;
        }

        uid_t peer_uid = ::getuid();
        pid_t peer_pid = 0;
#ifdef SO_PEERCRED
        ucred credentials{};
        socklen_t credentials_size = sizeof(credentials);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0 ||
            credentials.uid != ::getuid()) {
            std::cerr << "[eventd] rejected foreign or unverifiable peer\n";
            ::close(fd);
            continue;
        }
        peer_uid = credentials.uid;
        peer_pid = credentials.pid;
#endif

        if (!running_.load()) {
            ::close(fd);
            break;
        }
        auto client = std::make_shared<ClientConnection>();
        client->fd = fd;
        client->peer_uid = peer_uid;
        client->peer_pid = peer_pid;
        client->peer_executable = peer_executable(peer_pid);
        {
            std::lock_guard lock(clients_mutex_);
            if (!running_.load()) {
                ::close(fd);
                break;
            }
            clients_.push_back(client);
            client_threads_.emplace_back([this, client] { handle_client(client); });
        }
        prune_clients();
    }
}

void EventDaemonServer::stop() {
    if (!running_.exchange(false) && listen_fd_ < 0) return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(clients_mutex_);
        for (const auto& client : clients_) {
            client->alive.store(false);
            if (client->fd >= 0) ::shutdown(client->fd, SHUT_RDWR);
        }
        clients_.clear();
        threads.swap(client_threads_);
    }
    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
    if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
    persistence_.close();
    database_healthy_.store(false);
}

void EventDaemonServer::prune_clients() {
    std::lock_guard lock(clients_mutex_);
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(), [](const auto& client) {
            return !client->alive.load();
        }),
        clients_.end()
    );
}

std::size_t EventDaemonServer::subscriber_count() const {
    std::lock_guard lock(clients_mutex_);
    return static_cast<std::size_t>(std::count_if(clients_.begin(), clients_.end(), [](const auto& client) {
        return client->alive.load() && client->subscribed.load();
    }));
}

SourceTrust EventDaemonServer::classify_source(const ClientConnection& client, const std::string& source_id) const {
    if (client.peer_uid != ::getuid() || client.peer_pid <= 0 || client.peer_executable.empty()) {
        return SourceTrust::UntrustedLocal;
    }
    const std::string executable = std::filesystem::path(client.peer_executable).filename().string();
    // The generic CLI is intentionally only user-trusted: a user can freely
    // choose --source, so its source string is not a Realmheart identity proof.
    if (executable != "realmheart-event" && starts_with_realmheart(executable) && starts_with_realmheart(source_id)) {
        return SourceTrust::Realmheart;
    }
    return SourceTrust::User;
}

void EventDaemonServer::observe_source(
    const ClientConnection& client,
    const Source& source,
    SourceTrust trust
) {
    if (!persistence_.healthy()) return;
    const std::string now = realmheart::events::now_iso8601_utc();
    SourceRecord record;
    record.id = source.id;
    record.display_name = source.name.empty() ? source.id : source.name;
    record.icon = source.icon;
    record.uid = static_cast<std::uint32_t>(client.peer_uid);
    record.last_pid = static_cast<std::int64_t>(client.peer_pid);
    record.executable = client.peer_executable;
    record.trust_class = to_string(trust);
    record.first_seen = now;
    record.last_seen = now;
    std::string error;
    if (!persistence_.upsert_source(record, error)) {
        database_healthy_.store(false);
        std::cerr << "[eventd] unable to persist source metadata: " << error << '\n';
    }
}

void EventDaemonServer::persist_mutation(const std::string& op, const Event& event) {
    if (!persistence_.healthy()) return;
    std::string error;
    bool ok = true;
    if (op == "resolve") ok = persistence_.save_resolved(event, error);
    else if (op == "dismiss") ok = persistence_.save_dismissed(event, error);
    else if (op == "delete") ok = persistence_.erase({event.source.id, event.id}, error);
    else ok = persistence_.save_active(event, error);
    if (!ok) {
        database_healthy_.store(false);
        std::cerr << "[eventd] persistence failure during " << op << ": " << error << '\n';
    } else {
        database_healthy_.store(true);
    }
}

void EventDaemonServer::maybe_fallback(const Event& event) {
    if (subscriber_count() != 0U || event.severity != realmheart::events::Severity::Critical) return;
    const EventKey key{event.source.id, event.id};
    {
        std::lock_guard lock(fallback_mutex_);
        if (fallback_notified_.contains(key)) return;
        fallback_notified_.insert(key);
    }
    std::string error;
    if (!fallback_notifier_.notify(event, error)) {
        {
            std::lock_guard lock(fallback_mutex_);
            fallback_notified_.erase(key);
        }
        std::cerr << "[eventd] critical fallback notification failed: " << error << '\n';
    }
}

void EventDaemonServer::clear_fallback_marker(const EventKey& key) {
    std::lock_guard lock(fallback_mutex_);
    fallback_notified_.erase(key);
}

void EventDaemonServer::handle_client(const std::shared_ptr<ClientConnection>& client) {
    while (running_.load() && client->alive.load()) {
        std::string error;
        auto request = realmheart::events::receive_frame(client->fd, error);
        if (!request) break;

        std::optional<Json> delta;
        Json response = handle_request(client, *request, delta);
        {
            std::lock_guard lock(client->send_mutex);
            if (!realmheart::events::send_frame(client->fd, response, error)) break;
        }
        if (delta) broadcast(*delta);
    }
    client->alive.store(false);
    client->subscribed.store(false);
    if (client->fd >= 0) {
        ::shutdown(client->fd, SHUT_RDWR);
        ::close(client->fd);
        client->fd = -1;
    }
}

Json EventDaemonServer::handle_request(
    const std::shared_ptr<ClientConnection>& client,
    const Json& request,
    std::optional<Json>& delta
) {
    const ValidationResult envelope = realmheart::events::validate_request_envelope(request);
    if (!envelope.ok) return realmheart::events::error_response(envelope.code, envelope.message);
    const std::string op = request["op"].get<std::string>();

    if (op == "ping") {
        return realmheart::events::success_response(op, {{"status", "running"}, {"sequence", store_.sequence()}});
    }
    if (op == "status") {
        std::size_t source_count = 0;
        if (persistence_.healthy()) {
            std::string source_error;
            source_count = persistence_.sources(source_error).size();
        }
        return realmheart::events::success_response(op, {
            {"daemon", "running"},
            {"socket", socket_path_},
            {"database", persistence_.healthy() ? (database_healthy_.load() ? "healthy" : "degraded") : "memory-only"},
            {"database_path", persistence_.path()},
            {"active_events", store_.snapshot().size()},
            {"subscribers", subscriber_count()},
            {"sources", source_count},
            {"rate_limited_sources", rate_limiter_.rate_limited_sources()},
            {"sequence", store_.sequence()}
        });
    }
    if (op == "subscribe") {
        client->subscribed.store(true);
        Json events = Json::array();
        for (const auto& event : store_.snapshot()) events.push_back(realmheart::events::event_to_json(event));
        return {{"protocol", realmheart::events::kProtocolVersion}, {"type", "snapshot"}, {"sequence", store_.sequence()}, {"events", std::move(events)}};
    }
    if (op == "list") {
        Json events = Json::array();
        for (const auto& event : store_.snapshot()) events.push_back(realmheart::events::event_to_json(event));
        return realmheart::events::success_response(op, {{"events", std::move(events)}, {"sequence", store_.sequence()}});
    }
    if (op == "history") {
        if (!persistence_.healthy()) return realmheart::events::error_response("database_unavailable", "history is unavailable in memory-only mode");
        std::size_t limit = 100U;
        if (request.contains("limit")) {
            if (!request["limit"].is_number_unsigned() && !request["limit"].is_number_integer()) {
                return realmheart::events::error_response("invalid_limit", "history limit must be an integer");
            }
            const auto requested = request["limit"].get<long long>();
            if (requested <= 0) return realmheart::events::error_response("invalid_limit", "history limit must be positive");
            limit = static_cast<std::size_t>(requested);
        }
        std::string history_error;
        const auto history = persistence_.history(limit, history_error);
        if (!history_error.empty()) return realmheart::events::error_response("database_error", history_error);
        Json events = Json::array();
        for (const auto& event : history) events.push_back(realmheart::events::event_to_json(event));
        return realmheart::events::success_response(op, {{"events", std::move(events)}});
    }
    if (op == "clear_history") {
        if (!persistence_.healthy()) return realmheart::events::error_response("database_unavailable", "history is unavailable in memory-only mode");
        std::string clear_error;
        if (!persistence_.clear_history(clear_error)) return realmheart::events::error_response("database_error", clear_error);
        return realmheart::events::success_response(op, {{"status", "cleared"}});
    }
    if (op == "sources") {
        if (!persistence_.healthy()) return realmheart::events::error_response("database_unavailable", "source registry is unavailable in memory-only mode");
        std::string source_error;
        const auto sources = persistence_.sources(source_error);
        if (!source_error.empty()) return realmheart::events::error_response("database_error", source_error);
        Json encoded = Json::array();
        for (const auto& source : sources) encoded.push_back(source_record_to_json(source));
        return realmheart::events::success_response(op, {{"sources", std::move(encoded)}});
    }
    if (op == "register_source") {
        if (!request.contains("source_id") || !request["source_id"].is_string()) {
            return realmheart::events::error_response("missing_source", "register_source requires source_id");
        }
        const std::string source_id = request["source_id"].get<std::string>();
        if (source_id.empty() || source_id.size() > realmheart::events::kMaxSourceIdBytes) {
            return realmheart::events::error_response("invalid_source_id", "source id is empty or too large");
        }
        {
            std::lock_guard lock(clients_mutex_);
            for (const auto& existing : clients_) {
                if (existing.get() != client.get() && existing->alive.load() &&
                    existing->action_source_id == source_id) {
                    return realmheart::events::error_response("source_in_use", "source already has a registered action listener");
                }
            }
            client->action_source_id = source_id;
        }
        const SourceTrust trust = classify_source(*client, source_id);
        observe_source(*client, Source{source_id, source_id, ""}, trust);
        return realmheart::events::success_response(op, {{"source_id", source_id}, {"status", "registered"}, {"trust_class", to_string(trust)}});
    }

    const auto key_from_request = [&]() -> std::optional<EventKey> {
        if (!request.contains("source_id") || !request["source_id"].is_string() ||
            !request.contains("event_id") || !request["event_id"].is_string()) return std::nullopt;
        return EventKey{request["source_id"].get<std::string>(), request["event_id"].get<std::string>()};
    };

    if (op == "inspect") {
        const auto key = key_from_request();
        if (!key) return realmheart::events::error_response("missing_identity", "inspect requires source_id and event_id");
        const auto event = store_.inspect(*key);
        if (!event) return realmheart::events::error_response("not_found", "event does not exist");
        return realmheart::events::success_response(op, {{"event", realmheart::events::event_to_json(*event)}});
    }

    if (op == "invoke_action") {
        const auto key = key_from_request();
        if (!key) return realmheart::events::error_response("missing_identity", "invoke_action requires source_id and event_id");
        if (!request.contains("action_id") || !request["action_id"].is_string()) {
            return realmheart::events::error_response("missing_action", "invoke_action requires action_id");
        }
        const auto event = store_.inspect(*key);
        if (!event) return realmheart::events::error_response("not_found", "event does not exist");
        const std::string action_id = request["action_id"].get<std::string>();
        const auto action = std::find_if(event->actions.begin(), event->actions.end(), [&](const auto& candidate) {
            return candidate.id == action_id;
        });
        if (action == event->actions.end()) {
            return realmheart::events::error_response("action_not_found", "event does not expose that action");
        }
        if (action->kind != realmheart::events::ActionKind::Registered) {
            return realmheart::events::error_response("action_ui_owned", "uri and copy actions are handled by the Realmheart UI");
        }

        std::shared_ptr<ClientConnection> target;
        {
            std::lock_guard lock(clients_mutex_);
            for (const auto& candidate : clients_) {
                if (candidate->alive.load() && candidate->action_source_id == key->source_id) {
                    target = candidate;
                    break;
                }
            }
        }
        if (!target) {
            return realmheart::events::error_response("producer_unavailable", "registered action producer is not connected");
        }

        const std::uint64_t invocation_id = ++action_sequence_;
        Json invocation{
            {"protocol", realmheart::events::kProtocolVersion},
            {"type", "ACTION_INVOKED"},
            {"invocation_id", invocation_id},
            {"source_id", key->source_id},
            {"event_id", key->event_id},
            {"action_id", action_id}
        };
        std::string send_error;
        {
            std::lock_guard lock(target->send_mutex);
            if (!realmheart::events::send_frame(target->fd, invocation, send_error)) {
                target->alive.store(false);
                ::shutdown(target->fd, SHUT_RDWR);
                return realmheart::events::error_response("producer_unavailable", "registered action producer disconnected");
            }
        }
        return realmheart::events::success_response(op, {{"status", "routed"}, {"invocation_id", invocation_id}});
    }

    if (op == "create") {
        if (!request.contains("event")) return realmheart::events::error_response("missing_event", "create requires event");
        ValidationResult validation;
        auto event = realmheart::events::event_from_json(request["event"], validation);
        if (!event) return realmheart::events::error_response(validation.code, validation.message);
        const EventKey key{event->source.id, event->id};
        const bool replacement = store_.inspect(key).has_value();
        const SourceTrust trust = classify_source(*client, event->source.id);
        observe_source(*client, event->source, trust);

        const auto decision = replacement
            ? rate_limiter_.allow_update(event->source.id, trust)
            : rate_limiter_.allow_create(event->source.id, trust, store_.active_count_for_source(event->source.id));
        if (!decision.allowed) return realmheart::events::error_response("rate_limited", decision.reason);

        const auto result = store_.create(std::move(*event));
        if (!result.validation.ok || !result.event) return realmheart::events::error_response(result.validation.code, result.validation.message);
        persist_mutation("create", *result.event);
        maybe_fallback(*result.event);
        const Json encoded = realmheart::events::event_to_json(*result.event);
        delta = {{"protocol", realmheart::events::kProtocolVersion}, {"type", replacement ? "EVENT_UPDATED" : "EVENT_CREATED"}, {"sequence", result.event->revision}, {"event", encoded}};
        return realmheart::events::success_response(op, {{"event", encoded}});
    }

    const auto key = key_from_request();
    if (!key) return realmheart::events::error_response("missing_identity", op + " requires source_id and event_id");

    const auto current = store_.inspect(*key);
    if (op == "update" && current) {
        const SourceTrust trust = classify_source(*client, key->source_id);
        observe_source(*client, current->source, trust);
        const auto decision = rate_limiter_.allow_update(key->source_id, trust);
        if (!decision.allowed) return realmheart::events::error_response("rate_limited", decision.reason);
    }

    realmheart::events::MutationResult result;
    std::string delta_type;
    if (op == "update") {
        result = store_.update(*key, request.value("patch", Json::object()));
        delta_type = "EVENT_UPDATED";
    } else if (op == "resolve") {
        result = store_.resolve(*key, request.value("patch", Json::object()));
        delta_type = "EVENT_RESOLVED";
    } else if (op == "delete") {
        result = store_.remove(*key);
        delta_type = "EVENT_DELETED";
    } else if (op == "acknowledge") {
        result = store_.acknowledge(*key);
        delta_type = "EVENT_ACKNOWLEDGED";
    } else if (op == "dismiss") {
        result = store_.dismiss(*key);
        delta_type = "EVENT_DISMISSED";
    } else {
        return realmheart::events::error_response("unsupported_operation", "unsupported operation: " + op);
    }

    if (!result.validation.ok || !result.event) return realmheart::events::error_response(result.validation.code, result.validation.message);
    persist_mutation(op, *result.event);
    if (op == "update") maybe_fallback(*result.event);
    if (op == "resolve" || op == "delete" || op == "dismiss") clear_fallback_marker(*key);
    const Json encoded = realmheart::events::event_to_json(*result.event);
    delta = {{"protocol", realmheart::events::kProtocolVersion}, {"type", delta_type}, {"sequence", result.event->revision}, {"event", encoded}};
    return realmheart::events::success_response(op, {{"event", encoded}});
}

void EventDaemonServer::broadcast(const Json& message) {
    std::vector<std::shared_ptr<ClientConnection>> subscribers;
    {
        std::lock_guard lock(clients_mutex_);
        for (const auto& client : clients_) {
            if (client->alive.load() && client->subscribed.load()) subscribers.push_back(client);
        }
    }

    for (const auto& client : subscribers) {
        std::string error;
        std::lock_guard lock(client->send_mutex);
        if (!realmheart::events::send_frame(client->fd, message, error)) {
            client->alive.store(false);
            ::shutdown(client->fd, SHUT_RDWR);
        }
    }
}

} // namespace realmheart::eventd
