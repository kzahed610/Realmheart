#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace realmheart::services {

class WallpaperService {
public:
    explicit WallpaperService(std::filesystem::path state_file = {});

    [[nodiscard]] bool validate_image(const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<std::filesystem::path> load_path() const;
    [[nodiscard]] bool persist_path(const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<std::filesystem::path> load_output_path(
        std::string_view connector
    ) const;
    [[nodiscard]] bool persist_output_path(
        std::string_view connector,
        const std::filesystem::path& path
    ) const;

    // Transitional compatibility for UtilityManager. ShellRuntime will take direct
    // ownership of wallpaper state/rendering when the native manager is integrated.
    bool set_wallpaper(const std::string& path);
    bool choose_wallpaper();
    bool generate_colors();
    bool update_state(const std::filesystem::path& path);

private:
    [[nodiscard]] std::filesystem::path output_state_file(
        std::string_view connector
    ) const;

    std::filesystem::path state_file_;
};

} // namespace realmheart::services
