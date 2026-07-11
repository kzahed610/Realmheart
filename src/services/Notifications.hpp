#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace realmheart::services {

struct NotificationLimits {
    static constexpr std::size_t max_app_name_bytes = 256;
    static constexpr std::size_t max_summary_bytes = 4 * 1024;
    static constexpr std::size_t max_body_bytes = 64 * 1024;
};

struct NotificationEntry {
    std::uint32_t id = 0;
    std::string app_name;
    std::string summary;
    std::string body;
    bool unread = true;
};

struct NotificationSnapshot {
    std::vector<NotificationEntry> entries;
    std::size_t unread_count = 0;
    bool capture_active = false;
};

void bound_notification_payload(NotificationEntry& entry);

class NotificationHistory {
public:
    explicit NotificationHistory(std::size_t max_entries = 100);

    void upsert(NotificationEntry entry);
    bool dismiss(std::uint32_t id);
    void mark_all_read();
    void clear();
    void set_capture_active(bool active);
    [[nodiscard]] NotificationSnapshot snapshot() const;

private:
    std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::vector<NotificationEntry> entries_;
    bool capture_active_ = false;
};

} // namespace realmheart::services
