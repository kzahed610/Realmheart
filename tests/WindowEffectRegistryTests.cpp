#include "WindowEffectRegistry.hpp"

#include <cassert>
#include <cmath>
#include <string_view>

int main() {
    const auto specs = windowEffectSpecs();
    assert(specs.size() == 2);

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

    assert(findWindowEffect(std::string_view{"missing-effect"}) == nullptr);
    return 0;
}
