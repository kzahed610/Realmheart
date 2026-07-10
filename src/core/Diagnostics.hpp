#pragma once

#include <string>
#include <vector>

namespace realmheart::core {

struct DependencyCheck {
    std::string name;
    std::string purpose;
    bool required = false;
    bool available = false;
    std::string path;
};

std::vector<DependencyCheck> collect_dependency_checks();
std::string format_dependency_report(const std::vector<DependencyCheck>& checks);

} // namespace realmheart::core
