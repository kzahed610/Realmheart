#pragma once

#include "services/Notifications.hpp"

#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>

namespace realmheart::services {

class NotificationServer {
public:
    using NotificationHandler = std::function<void(const NotificationEntry&)>;
    using TransientHandler = std::function<void(const NotificationEntry&, int)>;
    using ClosedHandler = std::function<void(std::uint32_t, std::uint32_t)>;

    explicit NotificationServer(NotificationHistory& history);

    std::uint32_t notify(
        std::string app_name,
        std::uint32_t replaces_id,
        std::string summary,
        std::string body,
        int timeout_ms = 5000
    );
    bool close(std::uint32_t id, std::uint32_t reason = 3);
    void set_notification_handler(NotificationHandler handler);
    void set_transient_handler(TransientHandler handler);
    void set_closed_handler(ClosedHandler handler);
    void set_closed_observer(ClosedHandler observer);

private:
    bool contains(std::uint32_t id) const;
    std::uint32_t allocate_id();

    NotificationHistory& history_;
    std::uint32_t next_id_ = 1;
    std::size_t max_active_ = 0;
    std::list<std::uint32_t> active_order_;
    std::unordered_map<std::uint32_t, std::list<std::uint32_t>::iterator> active_ids_;
    NotificationHandler notification_handler_;
    TransientHandler transient_handler_;
    ClosedHandler closed_handler_;
    ClosedHandler closed_observer_;
};

} // namespace realmheart::services
