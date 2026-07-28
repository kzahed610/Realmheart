#include "effects/core/EffectRegistry.hpp"

#include <array>

namespace realmheart::effects {
namespace {

constexpr EffectTargetMask kAllTargets =
    effect_target_bit(EffectTargetType::Launcher) |
    effect_target_bit(EffectTargetType::Sidebar) |
    effect_target_bit(EffectTargetType::Notification) |
    effect_target_bit(EffectTargetType::Clipboard) |
    effect_target_bit(EffectTargetType::EmojiPicker) |
    effect_target_bit(EffectTargetType::Popover) |
    effect_target_bit(EffectTargetType::LockScreen) |
    effect_target_bit(EffectTargetType::Workspace) |
    effect_target_bit(EffectTargetType::Window);

constexpr EffectTargetMask kShellSurfaceTargets =
    effect_target_bit(EffectTargetType::Launcher) |
    effect_target_bit(EffectTargetType::Sidebar) |
    effect_target_bit(EffectTargetType::Notification) |
    effect_target_bit(EffectTargetType::Clipboard) |
    effect_target_bit(EffectTargetType::EmojiPicker) |
    effect_target_bit(EffectTargetType::Popover);

constexpr std::array<EffectSpec, 3> kEffectSpecs{{
    {
        .id = EffectId::None,
        .name = "none",
        .backend = EffectBackend::None,
        .supported_targets = kAllTargets,
        .supports_open = true,
        .supports_close = true,
        .default_open_duration_seconds = 0.0,
        .default_close_duration_seconds = 0.0,
        .fragment_shader_asset = {},
        .requires_source_texture = false,
        .outputs_transparency = false,
    },
    {
        .id = EffectId::FadeScale,
        .name = "fade-scale",
        .backend = EffectBackend::SnapshotTransform,
        .supported_targets = kShellSurfaceTargets,
        .supports_open = true,
        .supports_close = true,
        .default_open_duration_seconds = 0.22,
        .default_close_duration_seconds = 0.16,
        .fragment_shader_asset = {},
        .requires_source_texture = false,
        .outputs_transparency = false,
    },
    {
        .id = EffectId::Void,
        .name = "void",
        .backend = EffectBackend::Shader,
        .supported_targets =
            effect_target_bit(EffectTargetType::Launcher) |
            effect_target_bit(EffectTargetType::Sidebar),
        .supports_open = true,
        .supports_close = true,
        .default_open_duration_seconds = 0.48,
        .default_close_duration_seconds = 0.40,
        .fragment_shader_asset = "void/void.frag",
        .requires_source_texture = true,
        .outputs_transparency = true,
    },
}};

} // namespace

std::span<const EffectSpec> effect_specs() noexcept {
    return kEffectSpecs;
}

const EffectSpec* find_effect(EffectId id) noexcept {
    for (const auto& effect : kEffectSpecs) {
        if (effect.id == id) return &effect;
    }
    return nullptr;
}

const EffectSpec* find_effect(std::string_view name) noexcept {
    for (const auto& effect : kEffectSpecs) {
        if (effect.name == name) return &effect;
    }
    return nullptr;
}

bool supports_target(
    const EffectSpec& effect,
    EffectTargetType target
) noexcept {
    return (effect.supported_targets & effect_target_bit(target)) != 0;
}

EffectId resolve_effect(
    EffectId requested,
    EffectTargetType target,
    EffectId fallback
) noexcept {
    if (const auto* effect = find_effect(requested);
        effect != nullptr && supports_target(*effect, target)) {
        return requested;
    }

    if (const auto* fallback_effect = find_effect(fallback);
        fallback_effect != nullptr && supports_target(*fallback_effect, target)) {
        return fallback;
    }

    return EffectId::None;
}

} // namespace realmheart::effects
