#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
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
private:
    struct SubscriberRegistry;

public:
    using ChangedCallback = std::function<void(const NotificationSnapshot&)>;

    class Subscription {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        void reset();

    private:
        friend class NotificationHistory;
        Subscription(std::weak_ptr<SubscriberRegistry> registry, std::size_t id)
            : registry_(std::move(registry)), id_(id) {}
        std::weak_ptr<SubscriberRegistry> registry_;
        std::size_t id_ = 0;
    };

    explicit NotificationHistory(std::size_t max_entries = 100);

    void upsert(NotificationEntry entry);
    bool dismiss(std::uint32_t id);
    void mark_all_read();
    void clear();
    void set_capture_active(bool active);
    [[nodiscard]] NotificationSnapshot snapshot() const;
    [[nodiscard]] Subscription subscribe(ChangedCallback callback);

private:
    struct SubscriberRegistry {
        std::mutex mutex;
        std::size_t next_id = 1;
        std::unordered_map<std::size_t, ChangedCallback> callbacks;
    };

    void publish(const NotificationSnapshot& snapshot);

    std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::vector<NotificationEntry> entries_;
    bool capture_active_ = false;
    std::shared_ptr<SubscriberRegistry> subscribers_ = std::make_shared<SubscriberRegistry>();
};

} // namespace realmheart::services
