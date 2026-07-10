#include "services/Notifications.hpp"

#include <algorithm>
#include <utility>

namespace realmheart::services {

NotificationHistory::NotificationHistory(std::size_t max_entries)
    : max_entries_(max_entries) {
    entries_.reserve(max_entries_);
}

void NotificationHistory::upsert(NotificationEntry entry) {
    std::scoped_lock lock(mutex_);
    const auto existing = std::find_if(entries_.begin(), entries_.end(), [&entry](const auto& current) {
        return current.id == entry.id;
    });
    if (existing != entries_.end()) entries_.erase(existing);

    if (max_entries_ == 0) return;
    if (entries_.size() == max_entries_) entries_.erase(entries_.begin());
    entries_.push_back(std::move(entry));
}

bool NotificationHistory::dismiss(std::uint32_t id) {
    std::scoped_lock lock(mutex_);
    const auto existing = std::find_if(entries_.begin(), entries_.end(), [id](const auto& entry) {
        return entry.id == id;
    });
    if (existing == entries_.end()) return false;
    entries_.erase(existing);
    return true;
}

void NotificationHistory::mark_all_read() {
    std::scoped_lock lock(mutex_);
    for (auto& entry : entries_) entry.unread = false;
}

void NotificationHistory::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
}

void NotificationHistory::set_capture_active(bool active) {
    std::scoped_lock lock(mutex_);
    capture_active_ = active;
}

NotificationSnapshot NotificationHistory::snapshot() const {
    std::scoped_lock lock(mutex_);
    NotificationSnapshot snapshot;
    snapshot.entries = entries_;
    snapshot.unread_count = static_cast<std::size_t>(std::count_if(
        entries_.begin(),
        entries_.end(),
        [](const auto& entry) { return entry.unread; }
    ));
    snapshot.capture_active = capture_active_;
    return snapshot;
}

} // namespace realmheart::services
