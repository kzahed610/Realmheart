#pragma once

#include <string>
#include <vector>

namespace realmheart::ui::lockscreen {

[[nodiscard]] inline std::vector<std::string> auth_helper_path_candidates(
    const std::string& executable_path,
    const std::string& relative_path,
    const std::string& configured_path
) {
    std::vector<std::string> candidates;
    const auto executable_slash = executable_path.find_last_of('/');
    if (executable_slash == std::string::npos) return candidates;

    const std::string executable_dir = executable_path.substr(0, executable_slash);
    if (!relative_path.empty() && relative_path.front() != '/') {
        const auto prefix_slash = executable_dir.find_last_of('/');
        if (prefix_slash != std::string::npos) {
            candidates.push_back(
                executable_dir.substr(0, prefix_slash) + "/" + relative_path
            );
        }
    }
    if (!configured_path.empty()) candidates.push_back(configured_path);

    // The privileged helper is system-installed even when a developer runs a
    // build-tree shell whose CMake prefix lives under the user's home.
    if (!relative_path.empty() && relative_path.front() != '/') {
        candidates.push_back("/usr/local/" + relative_path);
        candidates.push_back("/usr/" + relative_path);
    }

    candidates.push_back(executable_dir + "/realmheart-auth-helper");
    return candidates;
}

template <typename IsSecure>
[[nodiscard]] std::string first_secure_auth_helper_path(
    const std::vector<std::string>& candidates,
    IsSecure&& is_secure
) {
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && is_secure(candidate)) return candidate;
    }
    return {};
}

} // namespace realmheart::ui::lockscreen
