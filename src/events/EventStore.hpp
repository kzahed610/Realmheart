#pragma once

#include "events/EventProtocol.hpp"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace realmheart::events {

struct MutationResult {
    ValidationResult validation;
    std::optional<Event> event;
};

class EventStore {
public:
    MutationResult create(Event event);
    MutationResult update(const EventKey& key, const Json& patch);
    MutationResult resolve(const EventKey& key, const Json& patch = Json::object());
    MutationResult acknowledge(const EventKey& key);
    MutationResult dismiss(const EventKey& key);
    MutationResult remove(const EventKey& key);

    [[nodiscard]] std::vector<Event> snapshot() const;
    [[nodiscard]] std::optional<Event> inspect(const EventKey& key) const;
    [[nodiscard]] std::uint64_t sequence() const;
    [[nodiscard]] std::size_t active_count_for_source(const std::string& source_id) const;
    void restore(std::vector<Event> events, std::uint64_t sequence_floor);

private:
    std::uint64_t next_revision_locked();

    mutable std::mutex mutex_;
    std::unordered_map<EventKey, Event, EventKeyHash> active_;
    std::uint64_t sequence_ = 0;
};

} // namespace realmheart::events
