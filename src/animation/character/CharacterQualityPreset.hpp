#pragma once

#include "animation/character/CharacterHairMode.hpp"

#include <optional>
#include <string_view>

namespace realmheart::animation::character {

// Realmheart-owned visual quality preset. This is intentionally independent
// from the host's power-profile service; a future settings panel can choose a
// preset without changing the machine's CPU power policy.
enum class CharacterQualityPreset {
    PowerSaving,
    Balanced,
    Performance,
};

struct CharacterQualityPolicy {
    CharacterHairMode hair_mode = CharacterHairMode::Mesh;
};

[[nodiscard]] CharacterQualityPolicy character_quality_policy(
    CharacterQualityPreset preset
) noexcept;
[[nodiscard]] std::optional<CharacterQualityPreset>
parse_character_quality_preset(std::string_view value) noexcept;
[[nodiscard]] std::string_view character_quality_preset_name(
    CharacterQualityPreset preset
) noexcept;

} // namespace realmheart::animation::character
