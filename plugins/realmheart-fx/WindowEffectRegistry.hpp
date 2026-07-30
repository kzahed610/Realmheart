#pragma once

#include <cstdint>
#include <span>
#include <string_view>

enum class EWindowEffectId : std::uint8_t {
    None,
    Void,
};

enum class EWindowEffectCapability : std::uint32_t {
    SourceTexture = 1U << 0U,
    Texture2D = 1U << 1U,
    ExternalTexture = 1U << 2U,
    RoundedSource = 1U << 3U,
};

using WindowEffectCapabilityMask = std::uint32_t;

[[nodiscard]] constexpr WindowEffectCapabilityMask windowEffectCapabilityBit(
    EWindowEffectCapability capability
) noexcept {
    return static_cast<WindowEffectCapabilityMask>(capability);
}

struct SWindowEffectSpec {
    EWindowEffectId id = EWindowEffectId::None;
    std::string_view name = "none";
    std::string_view displayName = "None";
    std::string_view fragmentShaderAsset{};
    float openDurationSeconds = 0.0F;
    float closeDurationSeconds = 0.0F;
    bool reversible = false;
    WindowEffectCapabilityMask capabilities = 0U;
};

[[nodiscard]] std::span<const SWindowEffectSpec> windowEffectSpecs() noexcept;
[[nodiscard]] const SWindowEffectSpec* findWindowEffect(EWindowEffectId id) noexcept;
[[nodiscard]] const SWindowEffectSpec* findWindowEffect(std::string_view name) noexcept;
[[nodiscard]] bool windowEffectSupports(
    const SWindowEffectSpec& effect,
    EWindowEffectCapability capability
) noexcept;
