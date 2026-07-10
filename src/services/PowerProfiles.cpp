#include "services/PowerProfiles.hpp"

#include "core/Command.hpp"

#include <algorithm>

namespace realmheart::services {
namespace {

constexpr const char* kBusName = "org.freedesktop.UPower.PowerProfiles";
constexpr const char* kObjectPath = "/org/freedesktop/UPower/PowerProfiles";
constexpr const char* kInterface = "org.freedesktop.UPower.PowerProfiles";

std::optional<std::string> profile_from_busctl(const std::string& output) {
    const auto first_quote = output.find('"');
    if (first_quote == std::string::npos) return std::nullopt;
    const auto second_quote = output.find('"', first_quote + 1);
    if (second_quote == std::string::npos || second_quote == first_quote + 1) return std::nullopt;
    return output.substr(first_quote + 1, second_quote - first_quote - 1);
}

} // namespace

std::vector<std::string> PowerProfiles::cycle_order() {
    return {"power-saver", "balanced", "performance"};
}

std::string PowerProfiles::next_after(const std::string& current) {
    const auto order = cycle_order();
    auto it = std::find(order.begin(), order.end(), current);
    if (it == order.end() || ++it == order.end()) return order.front();
    return *it;
}

std::optional<std::string> PowerProfiles::current() {
    if (realmheart::core::command_exists("powerprofilesctl")) {
        const auto result = realmheart::core::run_capture({"powerprofilesctl", "get"});
        if (result.succeeded() && !result.truncated && !result.output.empty()) {
            return realmheart::core::trim(result.output);
        }
    }

    if (!realmheart::core::command_exists("busctl")) return std::nullopt;
    const auto result = realmheart::core::run_capture({
        "busctl", "get-property", kBusName, kObjectPath, kInterface, "ActiveProfile"
    });
    if (!result.succeeded() || result.truncated) return std::nullopt;
    return profile_from_busctl(result.output);
}

bool PowerProfiles::set(const std::string& profile) {
    bool written = false;
    if (realmheart::core::command_exists("powerprofilesctl")) {
        const auto result = realmheart::core::run_capture({"powerprofilesctl", "set", profile});
        written = result.succeeded();
    }
    if (!written && realmheart::core::command_exists("busctl")) {
        const auto result = realmheart::core::run_capture({
            "busctl", "set-property", kBusName, kObjectPath, kInterface,
            "ActiveProfile", "s", profile
        });
        written = result.succeeded();
    }
    if (!written) return false;

    const auto readback = current();
    return readback.has_value() && *readback == profile;
}

std::optional<std::string> PowerProfiles::cycle() {
    auto active = current();
    if (!active) return std::nullopt;
    auto next = next_after(*active);
    if (!set(next)) return std::nullopt;
    return next;
}

} // namespace realmheart::services
