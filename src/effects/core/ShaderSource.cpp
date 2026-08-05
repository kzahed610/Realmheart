#include "effects/core/ShaderSource.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>

#ifndef REALMHEART_INSTALL_EFFECT_DIR
#define REALMHEART_INSTALL_EFFECT_DIR ""
#endif

#ifndef REALMHEART_SOURCE_EFFECT_DIR
#define REALMHEART_SOURCE_EFFECT_DIR ""
#endif

namespace realmheart::effects {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

bool contains_symbol(std::string_view source, std::string_view symbol) noexcept {
    return source.find(symbol) != std::string_view::npos;
}

} // namespace

bool is_safe_shader_asset_path(std::string_view asset_path) noexcept {
    if (asset_path.empty()) return false;

    const std::filesystem::path path{asset_path};
    if (path.is_absolute()) return false;

    const auto normalized = path.lexically_normal();
    if (normalized.empty() || normalized == ".") return false;

    for (const auto& component : normalized) {
        if (component == "..") return false;
    }

    return normalized.extension() == ".frag";
}

std::vector<std::filesystem::path> shader_search_roots() {
    std::vector<std::filesystem::path> roots;
    roots.reserve(3);

    if (const char* override_dir = std::getenv("REALMHEART_EFFECT_DIR");
        override_dir != nullptr && *override_dir != '\0') {
        roots.emplace_back(override_dir);
    }

    if (std::string_view{REALMHEART_SOURCE_EFFECT_DIR}.empty() == false) {
        roots.emplace_back(REALMHEART_SOURCE_EFFECT_DIR);
    }

    if (std::string_view{REALMHEART_INSTALL_EFFECT_DIR}.empty() == false) {
        roots.emplace_back(REALMHEART_INSTALL_EFFECT_DIR);
    }

    return roots;
}

std::optional<ShaderSource> load_shader_source(
    std::string_view asset_path,
    std::string* error
) {
    if (!is_safe_shader_asset_path(asset_path)) {
        set_error(error, "unsafe or unsupported shader asset path");
        return std::nullopt;
    }

    const std::filesystem::path relative{asset_path};
    for (const auto& root : shader_search_roots()) {
        const auto candidate = root / relative;
        std::ifstream stream(candidate, std::ios::binary);
        if (!stream) continue;

        std::string text{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}
        };
        if (text.empty()) {
            set_error(error, "shader source is empty: " + candidate.string());
            return std::nullopt;
        }

        if (error != nullptr) error->clear();
        return ShaderSource{
            .path = candidate,
            .text = std::move(text),
        };
    }

    set_error(error, "shader asset not found: " + std::string{asset_path});
    return std::nullopt;
}

bool validate_shell_shader_contract(
    std::string_view source,
    std::string* missing_symbol
) noexcept {
    constexpr std::array<std::string_view, 10> kRequiredSymbols{{
        "uniform float progress",
        "uniform vec2 resolution",
        "uniform sampler2D tex",
        "uniform float radius",
        "uniform float reverse",
        "uniform vec3 uGold",
        "uniform vec3 uStarlight",
        "uniform vec3 uAstral",
        "uniform vec3 uVoid",
        "out vec4 fragColor",
    }};

    for (const auto symbol : kRequiredSymbols) {
        if (!contains_symbol(source, symbol)) {
            if (missing_symbol != nullptr) *missing_symbol = std::string{symbol};
            return false;
        }
    }

    if (missing_symbol != nullptr) missing_symbol->clear();
    return true;
}

bool validate_power_menu_ripple_shader_contract(
    std::string_view source,
    std::string* missing_symbol
) noexcept {
    constexpr std::array<std::string_view, 9> kRequiredSymbols{{
        "uniform float progress",
        "uniform vec2 resolution",
        "uniform sampler2D tex",
        "uniform vec2 origin",
        "uniform float opening",
        "uniform vec3 uGold",
        "uniform vec3 uStarlight",
        "uniform vec3 uAstral",
        "out vec4 fragColor",
    }};

    for (const auto symbol : kRequiredSymbols) {
        if (!contains_symbol(source, symbol)) {
            if (missing_symbol != nullptr) *missing_symbol = std::string{symbol};
            return false;
        }
    }

    if (missing_symbol != nullptr) missing_symbol->clear();
    return true;
}

} // namespace realmheart::effects
