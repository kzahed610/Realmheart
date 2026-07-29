#include "effects/core/EffectRegistry.hpp"

#include <cassert>
#include <string_view>

using realmheart::effects::EffectBackend;
using realmheart::effects::EffectId;
using realmheart::effects::EffectTargetType;
using realmheart::effects::effect_specs;
using realmheart::effects::find_effect;
using realmheart::effects::resolve_effect;
using realmheart::effects::supports_target;

int main() {
    const auto specs = effect_specs();
    assert(specs.size() == 3);

    const auto* none = find_effect(EffectId::None);
    assert(none != nullptr);
    assert(none->name == "none");
    assert(none->display_name == "None");
    assert(none->backend == EffectBackend::None);
    assert(supports_target(*none, EffectTargetType::Window));

    const auto* fade_scale = find_effect(std::string_view{"fade-scale"});
    assert(fade_scale != nullptr);
    assert(fade_scale->id == EffectId::FadeScale);
    assert(fade_scale->display_name == "Fade Scale");
    assert(fade_scale->backend == EffectBackend::SnapshotTransform);
    assert(supports_target(*fade_scale, EffectTargetType::Launcher));
    assert(supports_target(*fade_scale, EffectTargetType::Sidebar));
    assert(!supports_target(*fade_scale, EffectTargetType::Window));
    assert(fade_scale->default_open_duration_seconds >
        fade_scale->default_close_duration_seconds);


    const auto* void_effect = find_effect(EffectId::Void);
    assert(void_effect != nullptr);
    assert(void_effect->name == "void");
    assert(void_effect->display_name == "Realmheart Void");
    assert(void_effect->backend == EffectBackend::Shader);
    assert(void_effect->fragment_shader_asset == "void/void.frag");
    assert(void_effect->requires_source_texture);
    assert(void_effect->outputs_transparency);
    assert(supports_target(*void_effect, EffectTargetType::Launcher));
    assert(supports_target(*void_effect, EffectTargetType::Sidebar));
    assert(supports_target(*void_effect, EffectTargetType::Window));

    assert(find_effect(std::string_view{"missing-effect"}) == nullptr);

    assert(resolve_effect(
        EffectId::FadeScale,
        EffectTargetType::Launcher
    ) == EffectId::FadeScale);
    assert(resolve_effect(
        EffectId::FadeScale,
        EffectTargetType::Window
    ) == EffectId::None);
    assert(resolve_effect(
        EffectId::FadeScale,
        EffectTargetType::Window,
        EffectId::FadeScale
    ) == EffectId::None);
    assert(resolve_effect(
        EffectId::Void,
        EffectTargetType::Window
    ) == EffectId::Void);

    return 0;
}
