#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::effects {

struct ShaderSource {
    std::filesystem::path path;
    std::string text;
};

using ShaderSourceCallback = std::function<void(
    std::optional<ShaderSource>,
    std::string
)>;

[[nodiscard]] bool is_safe_shader_asset_path(
    std::string_view asset_path
) noexcept;

[[nodiscard]] std::vector<std::filesystem::path> shader_search_roots();

[[nodiscard]] std::optional<ShaderSource> load_shader_source(
    std::string_view asset_path,
    std::string* error = nullptr
);

// Loads a shader on the shared worker pool and delivers the result on the GTK
// main context. The callback is not invoked when the task cannot be queued.
[[nodiscard]] bool load_shader_source_async(
    std::string asset_path,
    ShaderSourceCallback callback,
    std::string coalesce_key = {}
);

[[nodiscard]] bool validate_shell_shader_contract(
    std::string_view source,
    std::string* missing_symbol = nullptr
) noexcept;

[[nodiscard]] bool validate_power_menu_ripple_shader_contract(
    std::string_view source,
    std::string* missing_symbol = nullptr
) noexcept;

[[nodiscard]] bool validate_workspace_morph_shader_contract(
    std::string_view source,
    std::string* missing_symbol = nullptr
) noexcept;

[[nodiscard]] bool validate_lockscreen_shader_contract(
    std::string_view source,
    std::string* missing_symbol = nullptr
) noexcept;

} // namespace realmheart::effects
