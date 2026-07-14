#include "ui/bar/WorkspaceWindowTracker.hpp"

#include <algorithm>
#include <unordered_set>

namespace realmheart::ui::bar {

void WorkspaceWindowTracker::apply(services::WorkspaceSnapshot& snapshot) {
    std::unordered_set<std::string> live_addresses;

    for (auto& workspace : snapshot.workspaces) {
        for (auto& window : workspace.window_details) {
            if (window.address.empty()) {
                window.address = window.app_id + "\x1f" + window.title;
            }
            live_addresses.insert(window.address);
            if (!sequence_by_address_.contains(window.address)) {
                sequence_by_address_.emplace(window.address, next_sequence_++);
            }
        }

        std::stable_sort(
            workspace.window_details.begin(),
            workspace.window_details.end(),
            [this](const auto& left, const auto& right) {
                return sequence_by_address_.at(left.address) < sequence_by_address_.at(right.address);
            }
        );
    }

    std::erase_if(sequence_by_address_, [&live_addresses](const auto& entry) {
        return !live_addresses.contains(entry.first);
    });
}

} // namespace realmheart::ui::bar
