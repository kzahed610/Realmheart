#include "WindowEffectRegistry.hpp"

#include <array>

namespace {

constexpr WindowEffectCapabilityMask kTexturedWindowCapabilities =
    windowEffectCapabilityBit(EWindowEffectCapability::SourceTexture) |
    windowEffectCapabilityBit(EWindowEffectCapability::Texture2D) |
    windowEffectCapabilityBit(EWindowEffectCapability::ExternalTexture) |
    windowEffectCapabilityBit(EWindowEffectCapability::RoundedSource);

constexpr std::array<SWindowEffectSpec, 2> kWindowEffectSpecs{{
    {
        .id = EWindowEffectId::None,
        .name = "none",
        .displayName = "None",
        .fragmentShaderAsset = {},
        .openDurationSeconds = 0.0F,
        .closeDurationSeconds = 0.0F,
        .reversible = true,
        .capabilities = 0U,
    },
    {
        .id = EWindowEffectId::Void,
        .name = "void",
        .displayName = "Realmheart Void",
        .fragmentShaderAsset = "void/void.frag",
        .openDurationSeconds = 0.85F,
        .closeDurationSeconds = 0.85F,
        .reversible = true,
        .capabilities = kTexturedWindowCapabilities,
    },
}};

} // namespace

std::span<const SWindowEffectSpec> windowEffectSpecs() noexcept {
    return kWindowEffectSpecs;
}

const SWindowEffectSpec* findWindowEffect(EWindowEffectId id) noexcept {
    for (const auto& effect : kWindowEffectSpecs) {
        if (effect.id == id)
            return &effect;
    }
    return nullptr;
}

const SWindowEffectSpec* findWindowEffect(std::string_view name) noexcept {
    for (const auto& effect : kWindowEffectSpecs) {
        if (effect.name == name)
            return &effect;
    }
    return nullptr;
}

bool windowEffectSupports(
    const SWindowEffectSpec& effect,
    EWindowEffectCapability capability
) noexcept {
    return (effect.capabilities & windowEffectCapabilityBit(capability)) != 0U;
}
