#include "services/NotificationServer.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace realmheart::services {

NotificationServer::NotificationServer(NotificationHistory& history)
    : history_(history), max_active_(history.capacity()) {}

std::uint32_t NotificationServer::notify(
    std::string app_name,
    std::uint32_t replaces_id,
    std::string summary,
    std::string body,
    int timeout_ms
) {
    // Freedesktop notification clients own the replacement identity: a
    // non-zero replaces_id is returned unchanged even when the old entry has
    // already expired or been evicted.
    const std::uint32_t id = replaces_id != 0 ? replaces_id : allocate_id();

    if (const auto existing = active_ids_.find(id); existing != active_ids_.end()) {
        active_order_.erase(existing->second);
        active_ids_.erase(existing);
    } else if (max_active_ != 0 && active_ids_.size() >= max_active_) {
        const std::uint32_t retired = active_order_.front();
        active_order_.pop_front();
        active_ids_.erase(retired);
        if (closed_handler_) closed_handler_(retired, 4);
        if (closed_observer_) closed_observer_(retired, 4);
    }

    if (max_active_ != 0) {
        active_order_.push_back(id);
        active_ids_.emplace(id, std::prev(active_order_.end()));
    }

    NotificationEntry entry;
    entry.id = id;
    entry.app_name = std::move(app_name);
    entry.summary = std::move(summary);
    entry.body = std::move(body);
    entry.unread = true;
    bound_notification_payload(entry);
    history_.upsert(entry);
    if (notification_handler_) notification_handler_(entry);
    if (transient_handler_) {
        transient_handler_(entry, timeout_ms < 0 ? 5000 : timeout_ms);
    }
    return id;
}

bool NotificationServer::close(std::uint32_t id, std::uint32_t reason) {
    // Closing a desktop notification ends its transient toast lifecycle, but
    // Realmheart's sidebar is notification history. History is only removed
    // by the user's dismiss/clear actions inside the sidebar.
    const auto existing = active_ids_.find(id);
    if (existing == active_ids_.end()) return false;
    active_order_.erase(existing->second);
    active_ids_.erase(existing);
    if (closed_handler_) closed_handler_(id, reason);
    if (closed_observer_) closed_observer_(id, reason);
    return true;
}

void NotificationServer::set_notification_handler(NotificationHandler handler) {
    notification_handler_ = std::move(handler);
}

void NotificationServer::set_transient_handler(TransientHandler handler) {
    transient_handler_ = std::move(handler);
}

void NotificationServer::set_closed_handler(ClosedHandler handler) {
    closed_handler_ = std::move(handler);
}

void NotificationServer::set_closed_observer(ClosedHandler observer) {
    closed_observer_ = std::move(observer);
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
