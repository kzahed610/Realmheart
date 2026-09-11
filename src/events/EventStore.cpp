#include "events/EventStore.hpp"

#include <algorithm>

namespace realmheart::events {

std::uint64_t EventStore::next_revision_locked() { return ++sequence_; }

MutationResult EventStore::create(Event event) {
    const auto validation = validate_create_event(event);
    if (!validation.ok) return {validation, std::nullopt};
    std::lock_guard lock(mutex_);
    const EventKey key{event.source.id, event.id};
    if (event.timestamp.empty()) event.timestamp = now_iso8601_utc();
    event.lifecycle.state = LifecycleState::Active;
    event.revision = next_revision_locked();
    active_[key] = event;
    return {ValidationResult::success(), event};
}

MutationResult EventStore::update(const EventKey& key, const Json& patch) {
    std::lock_guard lock(mutex_);
    auto it = active_.find(key);
    if (it == active_.end()) return {ValidationResult::failure("not_found", "event does not exist"), std::nullopt};
    Event updated = it->second;
    const auto validation = apply_event_patch(updated, patch);
    if (!validation.ok) return {validation, std::nullopt};
    updated.lifecycle.state = LifecycleState::Active;
    if (patch.contains("timestamp") == false) updated.timestamp = now_iso8601_utc();
    updated.revision = next_revision_locked();
    it->second = updated;
    return {ValidationResult::success(), updated};
}

MutationResult EventStore::resolve(const EventKey& key, const Json& patch) {
    std::lock_guard lock(mutex_);
    auto it = active_.find(key);
    if (it == active_.end()) return {ValidationResult::failure("not_found", "event does not exist"), std::nullopt};
    Event resolved = it->second;
    const auto validation = apply_event_patch(resolved, patch);
    if (!validation.ok) return {validation, std::nullopt};
    resolved.lifecycle.state = LifecycleState::Resolved;
    resolved.timestamp = now_iso8601_utc();
    resolved.revision = next_revision_locked();
    active_.erase(it);
    return {ValidationResult::success(), resolved};
}

MutationResult EventStore::acknowledge(const EventKey& key) {
    std::lock_guard lock(mutex_);
    auto it = active_.find(key);
    if (it == active_.end()) return {ValidationResult::failure("not_found", "event does not exist"), std::nullopt};
    it->second.lifecycle.acknowledged = true;
    it->second.revision = next_revision_locked();
    return {ValidationResult::success(), it->second};
}

MutationResult EventStore::dismiss(const EventKey& key) {
    std::lock_guard lock(mutex_);
    auto it = active_.find(key);
    if (it == active_.end()) return {ValidationResult::failure("not_found", "event does not exist"), std::nullopt};
    Event dismissed = it->second;
    dismissed.revision = next_revision_locked();
    active_.erase(it);
    return {ValidationResult::success(), dismissed};
}

MutationResult EventStore::remove(const EventKey& key) {
    return dismiss(key);
}

std::vector<Event> EventStore::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<Event> events;
    events.reserve(active_.size());
    for (const auto& [key, event] : active_) {
        static_cast<void>(key);
        events.push_back(event);
    }
    std::sort(events.begin(), events.end(), [](const Event& left, const Event& right) {
        return left.revision > right.revision;
    });
    return events;
}

std::optional<Event> EventStore::inspect(const EventKey& key) const {
    std::lock_guard lock(mutex_);
    const auto it = active_.find(key);
    if (it == active_.end()) return std::nullopt;
    return it->second;
}

std::size_t EventStore::active_count_for_source(const std::string& source_id) const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(active_.begin(), active_.end(), [&](const auto& entry) {
        return entry.first.source_id == source_id;
    }));
}

void EventStore::restore(std::vector<Event> events, std::uint64_t sequence_floor) {
    std::lock_guard lock(mutex_);
    active_.clear();
    sequence_ = sequence_floor;
    for (auto& event : events) {
        if (event.lifecycle.state != LifecycleState::Active || !event.lifecycle.persistent) continue;
        sequence_ = std::max(sequence_, event.revision);
        active_[EventKey{event.source.id, event.id}] = std::move(event);
    }
}

std::uint64_t EventStore::sequence() const {
    std::lock_guard lock(mutex_);
    return sequence_;
}

} // namespace realmheart::events
