#include "services/Notifications.hpp"

#include <algorithm>
#include <utility>

namespace realmheart::services {
namespace {

void truncate_utf8_at_boundary(std::string& value, std::size_t max_bytes) {
    if (value.size() <= max_bytes) return;
    std::size_t boundary = max_bytes;
    while (boundary > 0 &&
           (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
        --boundary;
    }
    value.resize(boundary);
}

NotificationSnapshot make_snapshot(
    const std::vector<NotificationEntry>& entries,
    bool capture_active
) {
    NotificationSnapshot snapshot;
    snapshot.entries = entries;
    snapshot.unread_count = static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(), [](const auto& entry) { return entry.unread; }
    ));
    snapshot.capture_active = capture_active;
    return snapshot;
}

} // namespace

void bound_notification_payload(NotificationEntry& entry) {
    truncate_utf8_at_boundary(entry.app_name, NotificationLimits::max_app_name_bytes);
    truncate_utf8_at_boundary(entry.summary, NotificationLimits::max_summary_bytes);
    truncate_utf8_at_boundary(entry.body, NotificationLimits::max_body_bytes);
}

NotificationHistory::Subscription::~Subscription() { reset(); }
NotificationHistory::Subscription::Subscription(Subscription&& other) noexcept
    : registry_(std::move(other.registry_)), id_(std::exchange(other.id_, 0)) {}
NotificationHistory::Subscription& NotificationHistory::Subscription::operator=(Subscription&& other) noexcept {
    if (this == &other) return *this;
    reset();
    registry_ = std::move(other.registry_);
    id_ = std::exchange(other.id_, 0);
    return *this;
}
void NotificationHistory::Subscription::reset() {
    if (id_ == 0) return;
    if (const auto registry = registry_.lock()) {
        std::lock_guard lock(registry->mutex);
        registry->callbacks.erase(id_);
    }
    registry_.reset();
    id_ = 0;
}

NotificationHistory::NotificationHistory(std::size_t max_entries)
    : max_entries_(max_entries) {
    entries_.reserve(max_entries_);
}

void NotificationHistory::upsert(NotificationEntry entry) {
    bound_notification_payload(entry);
    {
        std::scoped_lock lock(mutex_);
        const auto existing = std::find_if(entries_.begin(), entries_.end(), [&entry](const auto& current) {
            return current.id == entry.id;
        });
        if (existing != entries_.end()) entries_.erase(existing);
        if (max_entries_ != 0) {
            if (entries_.size() == max_entries_) entries_.erase(entries_.begin());
            entries_.push_back(std::move(entry));
        }
    }
    publish();
}

bool NotificationHistory::dismiss(std::uint32_t id) {
    {
        std::scoped_lock lock(mutex_);
        const auto existing = std::find_if(entries_.begin(), entries_.end(), [id](const auto& entry) {
            return entry.id == id;
        });
        if (existing == entries_.end()) return false;
        entries_.erase(existing);
    }
    publish();
    return true;
}

void NotificationHistory::mark_all_read() {
    {
        std::scoped_lock lock(mutex_);
        for (auto& entry : entries_) entry.unread = false;
    }
    publish();
}

void NotificationHistory::clear() {
    {
        std::scoped_lock lock(mutex_);
        entries_.clear();
    }
    publish();
}

void NotificationHistory::set_capture_active(bool active) {
    {
        std::scoped_lock lock(mutex_);
        if (capture_active_ == active) return;
        capture_active_ = active;
    }
    publish();
}

NotificationSnapshot NotificationHistory::snapshot() const {
    std::scoped_lock lock(mutex_);
    return make_snapshot(entries_, capture_active_);
}

NotificationHistory::Subscription NotificationHistory::subscribe(ChangedCallback callback) {
    if (!callback) return {};
    std::lock_guard lock(subscribers_->mutex);
    const std::size_t id = subscribers_->next_id++;
    subscribers_->callbacks.emplace(id, std::move(callback));
    return Subscription{subscribers_, id};
}

void NotificationHistory::publish() {
    std::vector<ChangedCallback> callbacks;
    {
        std::lock_guard lock(subscribers_->mutex);
        callbacks.reserve(subscribers_->callbacks.size());
        for (const auto& [_, callback] : subscribers_->callbacks) callbacks.push_back(callback);
    }
    for (auto& callback : callbacks) {
        if (callback) callback();
    }
}

} // namespace realmheart::services
