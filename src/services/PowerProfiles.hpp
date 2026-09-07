#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>
#include <vector>

namespace realmheart::services {

enum class PowerProfileMutationStatus {
    Applied,
    NotApplied,
    Unknown,
};

struct PowerProfileMutationResult {
    PowerProfileMutationStatus status = PowerProfileMutationStatus::NotApplied;
    std::optional<std::string> observed_profile;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == PowerProfileMutationStatus::Applied;
    }
};

class PowerProfiles {
public:
    static std::vector<std::string> cycle_order();
    static std::string next_after(const std::string& current);
    static std::optional<std::string> current(
        const realmheart::core::CommandOptions& options = {}
    );
    static bool set(const std::string& profile);
    static PowerProfileMutationResult set_result(
        const std::string& profile,
        const realmheart::core::CommandOptions& options = {}
    );
    static std::optional<std::string> cycle(
        const realmheart::core::CommandOptions& options = {}
    );
    static PowerProfileMutationResult cycle_result(
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
