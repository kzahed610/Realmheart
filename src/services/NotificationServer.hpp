#pragma once

#include "services/Notifications.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace realmheart::services {

class NotificationServer {
public:
    using NotificationHandler = std::function<void(const NotificationEntry&)>;

    explicit NotificationServer(NotificationHistory& history);

    std::uint32_t notify(
        std::string app_name,
        std::uint32_t replaces_id,
        std::string summary,
        std::string body
    );
    bool close(std::uint32_t id);
    void set_notification_handler(NotificationHandler handler);

private:
    bool contains(std::uint32_t id) const;
    std::uint32_t allocate_id();

    NotificationHistory& history_;
    std::uint32_t next_id_ = 1;
    NotificationHandler notification_handler_;
};

} // namespace realmheart::services
