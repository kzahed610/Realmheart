#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::effects {

struct ShaderSource {
    std::filesystem::path path;
    std::string text;
};

[[nodiscard]] bool is_safe_shader_asset_path(
    std::string_view asset_path
) noexcept;

[[nodiscard]] std::vector<std::filesystem::path> shader_search_roots();

[[nodiscard]] std::optional<ShaderSource> load_shader_source(
    std::string_view asset_path,
    std::string* error = nullptr
);

[[nodiscard]] bool validate_shell_shader_contract(
    std::string_view source,
    std::string* missing_symbol = nullptr
) noexcept;

[[nodiscard]] bool validate_power_menu_ripple_shader_contract(
    std::string_view source,
    std::string* missing_symbol = nullptr
) noexcept;

} // namespace realmheart::effects
