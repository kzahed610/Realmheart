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

    active_ids_.insert(id);

    NotificationEntry entry;
    entry.id = id;
    entry.app_name = std::move(app_name);
    entry.summary = std::move(summary);
    entry.body = std::move(body);
    entry.unread = true;
    bound_notification_payload(entry);
    history_.upsert(entry);
    if (notification_handler_) notification_handler_(entry);
    return id;
}

bool NotificationServer::close(std::uint32_t id) {
    // Closing a desktop notification ends its transient toast lifecycle, but
    // Realmheart's sidebar is notification history. History is only removed
    // by the user's dismiss/clear actions inside the sidebar.
    return active_ids_.erase(id) != 0;
}

void NotificationServer::set_notification_handler(NotificationHandler handler) {
    notification_handler_ = std::move(handler);
}

bool NotificationServer::contains(std::uint32_t id) const {
    return active_ids_.contains(id);
}

std::uint32_t NotificationServer::allocate_id() {
    for (;;) {
        const std::uint32_t candidate = next_id_++;
        if (next_id_ == 0) next_id_ = 1;
        if (candidate != 0 && !contains(candidate)) return candidate;
    }
}

} // namespace realmheart::services
