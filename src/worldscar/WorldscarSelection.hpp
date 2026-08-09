#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace realmheart::worldscar {

struct WorldscarPreviewSet {
    // Worldscar currently renders previous_far as the tiny top chamber, then
    // previous / selected / next below it. next_far is the hidden look-ahead
    // slot for Down; previous_far_far is the matching hidden look-behind slot
    // for Up so either direction can begin with its incoming wallpaper resident.
    std::filesystem::path previous_far_far;
    std::filesystem::path previous_far;
    std::filesystem::path previous;
    std::filesystem::path selected;
    std::filesystem::path next;
    std::filesystem::path next_far;
    bool previous_far_far_available = false;
    bool previous_far_visible = false;
    bool previous_visible = false;
    bool next_visible = false;
    bool next_far_visible = false;
};

// Pure selection model for Worldscar. It owns no textures and performs no I/O;
// it only turns the deterministic wallpaper library into a wrap-around preview
// ring that excludes the wallpaper already committed beneath the overlay.
class WorldscarSelection {
public:
    [[nodiscard]] static std::optional<WorldscarSelection> create(
        const std::vector<std::filesystem::path>& library,
        const std::filesystem::path& current_wallpaper
    );

    [[nodiscard]] const std::filesystem::path& selected() const noexcept;
    [[nodiscard]] WorldscarPreviewSet preview() const;
    [[nodiscard]] std::size_t candidate_count() const noexcept;
    [[nodiscard]] std::size_t selected_index() const noexcept;

    // direction < 0 selects the previous candidate; direction > 0 selects the
    // next candidate. Zero is a no-op. Returns false only for an empty model.
    bool navigate(int direction) noexcept;

private:
    std::vector<std::filesystem::path> candidates_;
    std::size_t selected_index_ = 0;
};

} // namespace realmheart::worldscar
