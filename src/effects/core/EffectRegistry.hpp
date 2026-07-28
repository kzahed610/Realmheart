#pragma once

#include "effects/core/EffectFrame.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace realmheart::effects {

enum class EffectTargetType : std::uint8_t {
    Launcher,
    Sidebar,
    Notification,
    Clipboard,
    EmojiPicker,
    Popover,
    LockScreen,
    Workspace,
    Window,
};

enum class EffectBackend : std::uint8_t {
    None,
    SnapshotTransform,
    Shader,
};

using EffectTargetMask = std::uint32_t;

[[nodiscard]] constexpr EffectTargetMask effect_target_bit(
    EffectTargetType target
) noexcept {
    return EffectTargetMask{1} << static_cast<unsigned>(target);
}

struct EffectSpec {
    EffectId id = EffectId::None;
    std::string_view name = "none";
    EffectBackend backend = EffectBackend::None;
    EffectTargetMask supported_targets = 0;
    bool supports_open = true;
    bool supports_close = true;
    double default_open_duration_seconds = 0.0;
    double default_close_duration_seconds = 0.0;
    std::string_view fragment_shader_asset{};
    bool requires_source_texture = false;
    bool outputs_transparency = false;
};

[[nodiscard]] std::span<const EffectSpec> effect_specs() noexcept;

[[nodiscard]] const EffectSpec* find_effect(EffectId id) noexcept;
[[nodiscard]] const EffectSpec* find_effect(std::string_view name) noexcept;

[[nodiscard]] bool supports_target(
    const EffectSpec& effect,
    EffectTargetType target
) noexcept;

[[nodiscard]] EffectId resolve_effect(
    EffectId requested,
    EffectTargetType target,
    EffectId fallback = EffectId::None
) noexcept;

} // namespace realmheart::effects
