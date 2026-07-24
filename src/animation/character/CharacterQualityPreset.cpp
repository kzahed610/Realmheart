#include "animation/character/CharacterQualityPreset.hpp"

namespace realmheart::animation::character {

CharacterQualityPolicy character_quality_policy(
    CharacterQualityPreset preset
) noexcept {
    switch (preset) {
    case CharacterQualityPreset::PowerSaving:
        return {.hair_mode = CharacterHairMode::Static};
    case CharacterQualityPreset::Balanced:
        return {.hair_mode = CharacterHairMode::Mesh};
    case CharacterQualityPreset::Performance:
        return {.hair_mode = CharacterHairMode::MeshFlow};
    }
    return {.hair_mode = CharacterHairMode::Mesh};
}

std::optional<CharacterQualityPreset> parse_character_quality_preset(
    std::string_view value
) noexcept {
    if (value == "power-saving") return CharacterQualityPreset::PowerSaving;
    if (value == "balanced") return CharacterQualityPreset::Balanced;
    if (value == "performance") return CharacterQualityPreset::Performance;
    return std::nullopt;
}

std::string_view character_quality_preset_name(
    CharacterQualityPreset preset
) noexcept {
    switch (preset) {
    case CharacterQualityPreset::PowerSaving: return "power-saving";
    case CharacterQualityPreset::Balanced: return "balanced";
    case CharacterQualityPreset::Performance: return "performance";
    }
    return "balanced";
}

} // namespace realmheart::animation::character
