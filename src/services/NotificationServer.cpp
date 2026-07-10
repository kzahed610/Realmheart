#include "services/NotificationServer.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace realmheart::services {

NotificationServer::NotificationServer(NotificationHistory& history)
    : history_(history) {}

std::uint32_t NotificationServer::notify(
    std::string app_name,
    std::uint32_t replaces_id,
    std::string summary,
    std::string body
) {
    const std::uint32_t id = replaces_id != 0 && contains(replaces_id)
        ? replaces_id
        : allocate_id();

    NotificationEntry entry;
    entry.id = id;
    entry.app_name = std::move(app_name);
    entry.summary = std::move(summary);
    entry.body = std::move(body);
    entry.unread = true;
    history_.upsert(entry);
    if (notification_handler_) notification_handler_(entry);
    return id;
}

bool NotificationServer::close(std::uint32_t id) {
    return history_.dismiss(id);
}

void NotificationServer::set_notification_handler(NotificationHandler handler) {
    notification_handler_ = std::move(handler);
}

bool NotificationServer::contains(std::uint32_t id) const {
    const auto snapshot = history_.snapshot();
    return std::any_of(snapshot.entries.begin(), snapshot.entries.end(), [id](const auto& entry) {
        return entry.id == id;
    });
}

std::uint32_t NotificationServer::allocate_id() {
    for (;;) {
        const std::uint32_t candidate = next_id_++;
        if (next_id_ == 0) next_id_ = 1;
        if (candidate != 0 && !contains(candidate)) return candidate;
    }
}

} // namespace realmheart::services
