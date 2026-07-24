#include "animation/character/CharacterHairMode.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using realmheart::animation::character::CharacterHairMode;
using realmheart::animation::character::character_hair_mode_name;
using realmheart::animation::character::parse_character_hair_mode;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    require(parse_character_hair_mode("static") == CharacterHairMode::Static,
            "static mode must parse");
    require(parse_character_hair_mode("mesh") == CharacterHairMode::Mesh,
            "mesh mode must parse");
    require(parse_character_hair_mode("mesh-flow") == CharacterHairMode::MeshFlow,
            "mesh-flow mode must parse");
    require(!parse_character_hair_mode("flow"), "unsupported shorthand must fail");
    require(character_hair_mode_name(CharacterHairMode::MeshFlow) == "mesh-flow",
            "mode names must round-trip");
    std::cout << "Character hair mode tests passed\n";
}
