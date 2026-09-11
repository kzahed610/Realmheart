#pragma once

#include "events/EventTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace realmheart::events {

struct SourceRecord {
    std::string id;
    std::string display_name;
    std::string icon;
    std::uint32_t uid = 0;
    std::int64_t last_pid = 0;
    std::string executable;
    std::string trust_class = "user";
    std::string first_seen;
    std::string last_seen;
};

class EventPersistence {
public:
    EventPersistence() = default;
    ~EventPersistence();
    EventPersistence(const EventPersistence&) = delete;
    EventPersistence& operator=(const EventPersistence&) = delete;

    bool open(std::string& error);
    void close();
    [[nodiscard]] bool healthy() const noexcept { return database_ != nullptr; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    bool save_active(const Event& event, std::string& error);
    bool save_resolved(const Event& event, std::string& error);
    bool save_dismissed(const Event& event, std::string& error);
    bool erase(const EventKey& key, std::string& error);

    [[nodiscard]] std::vector<Event> load_active(std::string& error) const;
    [[nodiscard]] std::vector<Event> history(std::size_t limit, std::string& error) const;
    [[nodiscard]] std::uint64_t max_revision(std::string& error) const;
    bool clear_history(std::string& error);
    bool cleanup_history(int max_days, std::size_t max_events, std::string& error);

    bool upsert_source(const SourceRecord& source, std::string& error);
    [[nodiscard]] std::vector<SourceRecord> sources(std::string& error) const;

    static std::string default_database_path();

private:
    bool migrate(std::string& error);
    bool save_event(const Event& event, bool dismissed, std::string& error);

    sqlite3* database_ = nullptr;
    std::string path_;
};

} // namespace realmheart::events
