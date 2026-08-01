#include "WindowEffectRegistry.hpp"

#include <cassert>
#include <cmath>
#include <string_view>

int main() {
    const auto specs = windowEffectSpecs();
    assert(specs.size() == 3);

    const auto* none = findWindowEffect(EWindowEffectId::None);
    assert(none != nullptr);
    assert(none->name == "none");
    assert(none->fragmentShaderAsset.empty());
    assert(none->openDurationSeconds == 0.0F);
    assert(none->closeDurationSeconds == 0.0F);

    const auto* voidEffect = findWindowEffect(std::string_view{"void"});
    assert(voidEffect != nullptr);
    assert(voidEffect->id == EWindowEffectId::Void);
    assert(voidEffect->displayName == "Realmheart Void");
    assert(voidEffect->fragmentShaderAsset == "void/void.frag");
    assert(std::abs(voidEffect->openDurationSeconds - 0.85F) < 0.0001F);
    assert(std::abs(voidEffect->closeDurationSeconds - 0.85F) < 0.0001F);
    assert(voidEffect->reversible);
    assert(windowEffectSupports(*voidEffect, EWindowEffectCapability::SourceTexture));
    assert(windowEffectSupports(*voidEffect, EWindowEffectCapability::Texture2D));
    assert(windowEffectSupports(*voidEffect, EWindowEffectCapability::ExternalTexture));
    assert(windowEffectSupports(*voidEffect, EWindowEffectCapability::RoundedSource));

    const auto* aetherSunder = findWindowEffect(std::string_view{"aether-sunder"});
    assert(aetherSunder != nullptr);
    assert(aetherSunder->id == EWindowEffectId::AetherSunder);
    assert(aetherSunder->displayName == "Aether Sunder");
    assert(aetherSunder->fragmentShaderAsset == "aether-sunder/aether-sunder.frag");
    assert(std::abs(aetherSunder->openDurationSeconds - 0.78F) < 0.0001F);
    assert(std::abs(aetherSunder->closeDurationSeconds - 0.78F) < 0.0001F);
    assert(aetherSunder->reversible);
    assert(windowEffectSupports(
        *aetherSunder,
        EWindowEffectCapability::SourceTexture
    ));
    assert(windowEffectSupports(
        *aetherSunder,
        EWindowEffectCapability::Texture2D
    ));
    assert(windowEffectSupports(
        *aetherSunder,
        EWindowEffectCapability::ExternalTexture
    ));
    assert(windowEffectSupports(
        *aetherSunder,
        EWindowEffectCapability::RoundedSource
    ));

    assert(findWindowEffect(EWindowEffectId::AetherSunder) == aetherSunder);
    assert(findWindowEffect(std::string_view{"missing-effect"}) == nullptr);
    return 0;
}
