#pragma once

#include <optional>
#include <string>
#include <vector>

namespace realmheart::services {

class PowerProfiles {
public:
    static std::vector<std::string> cycle_order();
    static std::string next_after(const std::string& current);
    static std::optional<std::string> current();
    static bool set(const std::string& profile);
    static std::optional<std::string> cycle();
};

} // namespace realmheart::services
