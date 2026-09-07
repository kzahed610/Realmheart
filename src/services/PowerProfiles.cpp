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

realmheart::core::CommandOptions operation_options(
    const realmheart::core::CommandOptions& input
) {
    auto options = input;
    if (options.deadline == 1500ms) options.deadline = 5s;
    const auto requested_deadline = std::chrono::steady_clock::now() + options.deadline;
    if (!options.deadline_at || requested_deadline < *options.deadline_at) {
        options.deadline_at = requested_deadline;
    }
    options.terminate_grace = std::max(options.terminate_grace, 250ms);
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

std::optional<std::string> current_from_powerprofilesctl(
    const realmheart::core::CommandOptions& options
) {
    if (!realmheart::core::command_exists("powerprofilesctl")) return std::nullopt;
    const auto result = realmheart::core::run_capture(
        {"powerprofilesctl", "get"}, options
    );
    if (!result.succeeded() || result.truncated || result.output.empty()) return std::nullopt;
    const auto profile = realmheart::core::trim(result.output);
    return valid_profile(profile) ? std::optional<std::string>{profile} : std::nullopt;
}

std::optional<std::string> current_from_busctl(
    const realmheart::core::CommandOptions& options
) {
    if (!realmheart::core::command_exists("busctl")) return std::nullopt;
    const auto result = realmheart::core::run_capture({
        "busctl", "get-property", kBusName, kObjectPath, kInterface, "ActiveProfile"
    }, options);
    if (!result.succeeded() || result.truncated) return std::nullopt;
    const auto profile = profile_from_busctl(result.output);
    return profile && valid_profile(*profile) ? profile : std::nullopt;
}

std::optional<std::string> current_with_options(
    const realmheart::core::CommandOptions& options
) {
    if (const auto profile = current_from_powerprofilesctl(options)) return profile;
    return current_from_busctl(options);
}

bool profile_matches(
    const std::string& profile,
    const realmheart::core::CommandOptions& options
) {
    const auto current = current_with_options(options);
    return current && *current == profile;
}

bool wait_for_profile(
    const std::string& profile,
    const realmheart::core::CommandOptions& options,
    std::chrono::milliseconds confirmation_window
) {
    auto bounded = options;
    const auto confirmation_deadline = std::chrono::steady_clock::now() + confirmation_window;
    if (!bounded.deadline_at || confirmation_deadline < *bounded.deadline_at) {
        bounded.deadline_at = confirmation_deadline;
    }
    while (!bounded.deadline_at || std::chrono::steady_clock::now() < *bounded.deadline_at) {
        if (profile_matches(profile, bounded)) return true;
        std::this_thread::sleep_for(60ms);
    }
    return false;
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

std::optional<std::string> PowerProfiles::current(
    const realmheart::core::CommandOptions& input
) {
    return current_with_options(operation_options(input));
}

PowerProfileMutationResult PowerProfiles::set_result(
    const std::string& profile,
    const realmheart::core::CommandOptions& input
) {
    PowerProfileMutationResult mutation;
    if (!valid_profile(profile)) {
        mutation.error = "Invalid power profile";
        return mutation;
    }

    const auto options = operation_options(input);
    if (profile_matches(profile, options)) {
        mutation.status = PowerProfileMutationStatus::Applied;
        mutation.observed_profile = profile;
        return mutation;
    }

    bool write_may_have_applied = false;
    std::string last_error;
    if (realmheart::core::command_exists("powerprofilesctl")) {
        const auto result = realmheart::core::run_capture(
            {"powerprofilesctl", "set", profile}, options
        );
        if (result.succeeded()) {
            write_may_have_applied = true;
            if (wait_for_profile(profile, options, 300ms)) {
                mutation.status = PowerProfileMutationStatus::Applied;
                mutation.observed_profile = profile;
                return mutation;
            }
        } else {
            last_error = realmheart::core::command_failure_detail(
                result,
                "powerprofilesctl set failed"
            );
        }
    }

    if (realmheart::core::command_exists("busctl")) {
        const auto result = realmheart::core::run_capture({
            "busctl", "set-property", kBusName, kObjectPath, kInterface,
            "ActiveProfile", "s", profile
        }, options);
        if (result.succeeded()) {
            write_may_have_applied = true;
            if (wait_for_profile(profile, options, 1200ms)) {
                mutation.status = PowerProfileMutationStatus::Applied;
                mutation.observed_profile = profile;
                return mutation;
            }
        } else if (last_error.empty()) {
            last_error = realmheart::core::command_failure_detail(
                result,
                "power profile D-Bus write failed"
            );
        }
    }

    mutation.status = write_may_have_applied
        ? PowerProfileMutationStatus::Unknown
        : PowerProfileMutationStatus::NotApplied;
    mutation.observed_profile = current_with_options(options);
    mutation.error = write_may_have_applied
        ? "Power profile write may have applied, but confirmation expired"
        : (last_error.empty() ? "Unable to set power profile" : last_error);
    return mutation;
}

bool PowerProfiles::set(const std::string& profile) {
    return set_result(profile).succeeded();
}

PowerProfileMutationResult PowerProfiles::cycle_result(
    const realmheart::core::CommandOptions& input
) {
    const auto options = operation_options(input);
    PowerProfileMutationResult mutation;
    const auto active = current_with_options(options);
    if (!active) {
        mutation.status = PowerProfileMutationStatus::Unknown;
        mutation.error = "Current power profile is unavailable";
        return mutation;
    }
    return set_result(next_after(*active), options);
}

std::optional<std::string> PowerProfiles::cycle(
    const realmheart::core::CommandOptions& options
) {
    const auto result = cycle_result(options);
    if (!result.succeeded()) return std::nullopt;
    return result.observed_profile;
}

} // namespace realmheart::services
