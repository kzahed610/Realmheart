#include "worldscar/WorldscarSelection.hpp"

#include <algorithm>
#include <system_error>

namespace realmheart::worldscar {
namespace {

bool same_path(
    const std::filesystem::path& left,
    const std::filesystem::path& right
) {
    std::error_code left_error;
    std::error_code right_error;
    const auto left_path = std::filesystem::weakly_canonical(left, left_error);
    const auto right_path = std::filesystem::weakly_canonical(right, right_error);
    if (!left_error && !right_error) return left_path == right_path;
    return left.lexically_normal() == right.lexically_normal();
}

} // namespace

std::optional<WorldscarSelection> WorldscarSelection::create(
    const std::vector<std::filesystem::path>& library,
    const std::filesystem::path& current_wallpaper
) {
    if (library.empty()) return std::nullopt;

    WorldscarSelection selection;
    selection.candidates_.reserve(library.size());

    std::size_t current_index = library.size();
    for (std::size_t index = 0; index < library.size(); ++index) {
        if (!current_wallpaper.empty() && same_path(library[index], current_wallpaper)) {
            current_index = index;
            continue;
        }
        selection.candidates_.push_back(library[index]);
    }

    // Preserve the one-image lifecycle/fallback case. There is no alternate
    // reality to browse, but opening/cancelling remains safe and deterministic.
    if (selection.candidates_.empty()) {
        selection.candidates_.push_back(library.front());
        selection.selected_index_ = 0;
        return selection;
    }

    // If the committed wallpaper lives in the deterministic library, begin at
    // the next alternate in that same order instead of jumping to an unrelated
    // first entry. The committed wallpaper itself stays outside the wound.
    if (current_index < library.size()) {
        for (std::size_t offset = 1; offset <= library.size(); ++offset) {
            const auto& candidate = library[(current_index + offset) % library.size()];
            const auto found = std::find_if(
                selection.candidates_.begin(),
                selection.candidates_.end(),
                [&](const std::filesystem::path& path) {
                    return same_path(path, candidate);
                }
            );
            if (found != selection.candidates_.end()) {
                selection.selected_index_ = static_cast<std::size_t>(
                    std::distance(selection.candidates_.begin(), found)
                );
                break;
            }
        }
    }

    return selection;
}

const std::filesystem::path& WorldscarSelection::selected() const noexcept {
    static const std::filesystem::path empty;
    if (candidates_.empty()) return empty;
    return candidates_[selected_index_ % candidates_.size()];
}

WorldscarPreviewSet WorldscarSelection::preview() const {
    WorldscarPreviewSet result;
    if (candidates_.empty()) return result;

    const std::size_t count = candidates_.size();
    const std::size_t selected_index = selected_index_ % count;
    result.selected = candidates_[selected_index];

    if (count == 1) return result;

    result.previous = candidates_[(selected_index + count - 1) % count];
    result.previous_visible = true;

    if (count >= 3) {
        result.next = candidates_[(selected_index + 1) % count];
        result.next_visible = true;
    } else {
        // With two alternatives, drawing the same neighbour twice makes the
        // wound look like duplicated cards. Keep one restrained neighbour.
        result.next = result.previous;
        result.next_visible = false;
        return result;
    }

    // Keep one invisible neighbour warm on each side. With four candidates the
    // two far roles point at the same path, which is harmless and still gives
    // either navigation direction a ready one-step cushion.
    if (count >= 4) {
        result.previous_far = candidates_[(selected_index + count - 2) % count];
        result.next_far = candidates_[(selected_index + 2) % count];
        result.previous_far_visible = true;
        result.next_far_visible = true;
    }
    return result;
}

std::size_t WorldscarSelection::candidate_count() const noexcept {
    return candidates_.size();
}

std::size_t WorldscarSelection::selected_index() const noexcept {
    return selected_index_;
}

bool WorldscarSelection::navigate(int direction) noexcept {
    if (candidates_.empty()) return false;
    if (direction < 0) {
        selected_index_ = (selected_index_ + candidates_.size() - 1) % candidates_.size();
    } else if (direction > 0) {
        selected_index_ = (selected_index_ + 1) % candidates_.size();
    }
    return true;
}

} // namespace realmheart::worldscar
