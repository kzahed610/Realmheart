#include "services/PowerProfiles.hpp"

#include "core/Command.hpp"

#include <algorithm>

namespace realmheart::services {

std::vector<std::string> PowerProfiles::cycle_order() {
    return {"battery-saver", "balanced", "performance"};
}

std::string PowerProfiles::next_after(const std::string& current) {
    const auto order = cycle_order();
    auto it = std::find(order.begin(), order.end(), current);
    if (it == order.end() || ++it == order.end()) return order.front();
    return *it;
}

std::optional<std::string> PowerProfiles::current() {
    if (!realmheart::core::command_exists("powerprofilesctl")) return std::nullopt;
    auto result = realmheart::core::run_capture({"powerprofilesctl", "get"});
    if (!result.succeeded() || result.truncated || result.output.empty()) return std::nullopt;
    return result.output;
}

bool PowerProfiles::set(const std::string& profile) {
    if (!realmheart::core::command_exists("powerprofilesctl")) return false;
    auto result = realmheart::core::run_capture({"powerprofilesctl", "set", profile});
    return result.succeeded();
}

std::optional<std::string> PowerProfiles::cycle() {
    auto active = current();
    if (!active) return std::nullopt;
    auto next = next_after(*active);
    if (!set(next)) return std::nullopt;
    return next;
}

} // namespace realmheart::services
