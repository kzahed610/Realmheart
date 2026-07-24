#include "animation/character/CharacterQualityPreset.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using realmheart::animation::character::CharacterHairMode;
using realmheart::animation::character::CharacterQualityPreset;
using realmheart::animation::character::character_quality_policy;
using realmheart::animation::character::character_quality_preset_name;
using realmheart::animation::character::parse_character_quality_preset;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_quality_policy_mapping() {
    require(
        character_quality_policy(CharacterQualityPreset::PowerSaving).hair_mode ==
            CharacterHairMode::Static,
        "power-saving preset must select static hair"
    );
    require(
        character_quality_policy(CharacterQualityPreset::Balanced).hair_mode ==
            CharacterHairMode::Mesh,
        "balanced preset must select mesh hair"
    );
    require(
        character_quality_policy(CharacterQualityPreset::Performance).hair_mode ==
            CharacterHairMode::MeshFlow,
        "performance preset must select mesh-flow hair"
    );
}

void test_quality_preset_serialization() {
    require(
        parse_character_quality_preset("power-saving") ==
            CharacterQualityPreset::PowerSaving,
        "power-saving preset must parse"
    );
    require(
        parse_character_quality_preset("balanced") ==
            CharacterQualityPreset::Balanced,
        "balanced preset must parse"
    );
    require(
        parse_character_quality_preset("performance") ==
            CharacterQualityPreset::Performance,
        "performance preset must parse"
    );
    require(
        !parse_character_quality_preset("powersave"),
        "unsupported preset aliases must fail"
    );
    require(
        character_quality_preset_name(CharacterQualityPreset::Performance) ==
            "performance",
        "preset names must remain stable for a future settings panel"
    );
}

} // namespace

int main() {
    test_quality_policy_mapping();
    test_quality_preset_serialization();
    std::cout << "Character quality preset tests passed\n";
    return 0;
}
