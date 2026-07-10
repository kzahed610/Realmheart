#include "services/Notifications.hpp"
#include "core/Command.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>

namespace realmheart::services {

void parse_notification_line(const std::string& line, NotificationHistory& history) {
    if (line.find("string \"") != std::string::npos) {
        size_t start = line.find("string \"") + 8;
        size_t end = line.find("\"", start);
        if (end != std::string::npos) {
            std::string content = line.substr(start, end - start);
            NotificationEntry entry;
            entry.id = static_cast<std::uint32_t>(std::hash<std::string>{}(content));
            entry.app_name = "System";
            entry.summary = content; 
            entry.body = "Notification captured via DBus";
            history.upsert(entry);
        }
    }
}

void run_notification_capture(NotificationHistory& history) {
    std::cout << "Starting DBus notification capture..." << std::endl;
    //- Implementation of the DBus monitor loop would go here.
}

} // namespace realmheart::services
