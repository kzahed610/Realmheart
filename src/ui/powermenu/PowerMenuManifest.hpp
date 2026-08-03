#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::ui::powermenu {

struct PowerMenuCanvas {
    int width = 0;
    int height = 0;
};

struct PowerMenuPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct PowerMenuAsset {
    std::string name;
    std::string category;
    std::string file_1x;
    std::filesystem::path path_1x;
    PowerMenuCanvas canvas_1x;
    PowerMenuPlacement placement_1x;
};

class PowerMenuManifest {
public:
    static std::optional<PowerMenuManifest> load(
        const std::filesystem::path& manifest_path,
        std::string* error_message = nullptr
    );

    [[nodiscard]] const PowerMenuAsset* find_asset(std::string_view name) const;

    std::filesystem::path root;
    PowerMenuCanvas logical_canvas;
    std::vector<PowerMenuAsset> assets;
};

} // namespace realmheart::ui::powermenu
