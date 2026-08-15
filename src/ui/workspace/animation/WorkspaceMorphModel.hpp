#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace realmheart::ui::workspace::animation {

inline constexpr std::size_t kWorkspaceMorphBandCount = 4;

struct WorkspaceMorphRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] double center_x() const noexcept {
        return x + width * 0.5;
    }

    [[nodiscard]] double center_y() const noexcept {
        return y + height * 0.5;
    }
};

struct WorkspaceMorphSource {
    int workspace_id = 0;
    WorkspaceMorphRect bounds{};
    bool active = false;
    bool occupied = false;
};

struct WorkspaceMorphBand {
    int workspace_id = 0;
    std::size_t style_index = 0;
    WorkspaceMorphRect source{};
    WorkspaceMorphRect destination{};
    bool active = false;
    bool occupied = false;
};

struct WorkspaceMorphLayout {
    std::array<WorkspaceMorphBand, kWorkspaceMorphBandCount> bands{};
    double width = 0.0;
    double height = 0.0;
};

struct WorkspaceMorphBandFrame {
    WorkspaceMorphRect reveal_clip{};
    WorkspaceMorphRect rune{};
    WorkspaceMorphRect proxy{};
    double realm_opacity = 0.0;
    double seed_opacity = 0.0;
    double rune_opacity = 0.0;
    double stroke_opacity = 0.0;
    double proxy_opacity = 0.0;
    double identity_opacity = 0.0;
    double character_opacity = 0.0;
    double card_opacity = 0.0;
};

struct WorkspaceMorphFrame {
    std::array<WorkspaceMorphBandFrame, kWorkspaceMorphBandCount> bands{};
    double separator_opacity = 0.0;
    double reveal_right = 0.0;
    bool exact_hidden = true;
    bool exact_visible = false;
};

[[nodiscard]] std::vector<WorkspaceMorphSource>
scale_workspace_morph_sources_to_reference(
    const std::vector<WorkspaceMorphSource>& sources,
    double scale_x,
    double scale_y
) noexcept;

[[nodiscard]] WorkspaceMorphLayout build_workspace_morph_layout(
    const std::array<int, kWorkspaceMorphBandCount>& workspace_ids,
    const std::array<double, kWorkspaceMorphBandCount>& destination_heights,
    const std::vector<WorkspaceMorphSource>& sources,
    double width,
    double height
) noexcept;

[[nodiscard]] WorkspaceMorphFrame sample_workspace_morph_frame(
    const WorkspaceMorphLayout& layout,
    double progress
) noexcept;

[[nodiscard]] double workspace_morph_rune_opacity(double progress) noexcept;

[[nodiscard]] double workspace_morph_stage(
    double progress,
    double start,
    double end
) noexcept;

} // namespace realmheart::ui::workspace::animation
