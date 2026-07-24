#include "animation/character/CharacterHairMode.hpp"

namespace realmheart::animation::character {

std::optional<CharacterHairMode> parse_character_hair_mode(
    std::string_view value
) noexcept {
    if (value == "static") return CharacterHairMode::Static;
    if (value == "mesh") return CharacterHairMode::Mesh;
    if (value == "mesh-flow") return CharacterHairMode::MeshFlow;
    return std::nullopt;
}

std::string_view character_hair_mode_name(CharacterHairMode mode) noexcept {
    switch (mode) {
    case CharacterHairMode::Static: return "static";
    case CharacterHairMode::Mesh: return "mesh";
    case CharacterHairMode::MeshFlow: return "mesh-flow";
    }
    return "mesh";
}

} // namespace realmheart::animation::character
