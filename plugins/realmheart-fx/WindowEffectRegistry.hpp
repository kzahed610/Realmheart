#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

inline constexpr std::string_view kNoWindowEffect = "none";


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
    std::string name = std::string{kNoWindowEffect};
    std::string displayName = "None";
    std::filesystem::path fragmentShaderAsset{};
    float openDurationSeconds = 0.0F;
    float closeDurationSeconds = 0.0F;
    bool reversible = false;
    WindowEffectCapabilityMask capabilities = 0U;
};

struct SWindowEffectRegistryLoadResult {
    bool success = false;
    std::size_t loadedEffects = 0U;
    std::string error;
};

[[nodiscard]] SWindowEffectRegistryLoadResult loadWindowEffectRegistry(
    const std::filesystem::path& effectRoot
);
[[nodiscard]] std::span<const SWindowEffectSpec> windowEffectSpecs() noexcept;
[[nodiscard]] const SWindowEffectSpec* findWindowEffect(std::string_view name) noexcept;
[[nodiscard]] bool windowEffectIsNone(const SWindowEffectSpec& effect) noexcept;
[[nodiscard]] bool windowEffectSupports(
    const SWindowEffectSpec& effect,
    EWindowEffectCapability capability
) noexcept;
