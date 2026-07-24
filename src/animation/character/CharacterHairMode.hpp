#pragma once

#include <optional>
#include <string_view>

namespace realmheart::animation::character {

enum class CharacterHairMode {
    Static,
    Mesh,
    MeshFlow,
};

[[nodiscard]] std::optional<CharacterHairMode> parse_character_hair_mode(
    std::string_view value
) noexcept;
[[nodiscard]] std::string_view character_hair_mode_name(
    CharacterHairMode mode
) noexcept;

} // namespace realmheart::animation::character
