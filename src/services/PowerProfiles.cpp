#include "services/PowerProfiles.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace realmheart::services {
namespace {

using namespace std::chrono_literals;

constexpr const char* kBusName = "org.freedesktop.UPower.PowerProfiles";
constexpr const char* kObjectPath = "/org/freedesktop/UPower/PowerProfiles";
constexpr const char* kInterface = "org.freedesktop.UPower.PowerProfiles";

realmheart::core::CommandOptions command_options() {
    realmheart::core::CommandOptions options;
    // Profile changes may need to wake/activate the system daemon. The generic
    // 1.5 second command deadline was short enough to silently kill that path
    // on some machines.
    options.deadline = 5s;
    options.terminate_grace = 250ms;
    return options;
}

std::optional<std::string> profile_from_busctl(const std::string& output) {
    const auto first_quote = output.find('"');
    if (first_quote == std::string::npos) return std::nullopt;
    const auto second_quote = output.find('"', first_quote + 1);
    if (second_quote == std::string::npos || second_quote == first_quote + 1) {
        return std::nullopt;
    }
    return output.substr(first_quote + 1, second_quote - first_quote - 1);
}

bool valid_profile(const std::string& profile) {
    const auto order = PowerProfiles::cycle_order();
    return std::find(order.begin(), order.end(), profile) != order.end();
}

std::optional<std::string> current_from_powerprofilesctl() {
    if (!realmheart::core::command_exists("powerprofilesctl")) return std::nullopt;
    const auto result = realmheart::core::run_capture(
        {"powerprofilesctl", "get"}, command_options()
    );
    if (!result.succeeded() || result.truncated || result.output.empty()) {
        return std::nullopt;
    }
    const auto profile = realmheart::core::trim(result.output);
    return valid_profile(profile) ? std::optional<std::string>{profile} : std::nullopt;
}

std::optional<std::string> current_from_busctl() {
    if (!realmheart::core::command_exists("busctl")) return std::nullopt;
    const auto result = realmheart::core::run_capture({
        "busctl", "get-property", kBusName, kObjectPath, kInterface, "ActiveProfile"
    }, command_options());
    if (!result.succeeded() || result.truncated) return std::nullopt;
    const auto profile = profile_from_busctl(result.output);
    return profile && valid_profile(*profile) ? profile : std::nullopt;
}

bool profile_matches(const std::string& profile) {
    const auto via_cli = current_from_powerprofilesctl();
    if (via_cli && *via_cli == profile) return true;
    const auto via_bus = current_from_busctl();
    return via_bus && *via_bus == profile;
}

bool wait_for_profile(const std::string& profile, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (profile_matches(profile)) return true;
        std::this_thread::sleep_for(60ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return profile_matches(profile);
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
    if (const auto profile = current_from_powerprofilesctl()) return profile;
    return current_from_busctl();
}

bool PowerProfiles::set(const std::string& profile) {
    if (!valid_profile(profile)) return false;
    if (profile_matches(profile)) return true;

    // Prefer the supported CLI, but do not treat a zero exit status as proof
    // that the daemon actually applied the profile. Confirm through readback.
    // If that path lies or races, retry through the writable D-Bus property.
    if (realmheart::core::command_exists("powerprofilesctl")) {
        const auto result = realmheart::core::run_capture(
            {"powerprofilesctl", "set", profile}, command_options()
        );
        if (result.succeeded() && wait_for_profile(profile, 300ms)) return true;
    }

    if (realmheart::core::command_exists("busctl")) {
        const auto result = realmheart::core::run_capture({
            "busctl", "set-property", kBusName, kObjectPath, kInterface,
            "ActiveProfile", "s", profile
        }, command_options());
        if (result.succeeded() && wait_for_profile(profile, 1200ms)) return true;
    }

    return false;
}

std::optional<std::string> PowerProfiles::cycle() {
    const auto active = current();
    if (!active) return std::nullopt;
    const auto next = next_after(*active);
    if (!set(next)) return std::nullopt;
    return next;
}

} // namespace realmheart::services
