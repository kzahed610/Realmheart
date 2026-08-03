#include "ui/powermenu/PowerMenuManifest.hpp"

#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace realmheart::ui::powermenu {
namespace {

using Json = nlohmann::json;

void set_error(std::string* target, std::string message) {
    if (target != nullptr) *target = std::move(message);
}

std::optional<Json> read_json(
    const std::filesystem::path& path,
    std::string* error_message
) {
    std::ifstream input(path);
    if (!input) {
        set_error(error_message, "Unable to open power-menu manifest: " + path.string());
        return std::nullopt;
    }

    try {
        return Json::parse(input);
    } catch (const std::exception& error) {
        set_error(
            error_message,
            "Unable to parse power-menu manifest " + path.string() + ": " + error.what()
        );
        return std::nullopt;
    }
}

bool path_is_inside(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    const auto mismatch = std::mismatch(
        root.begin(), root.end(), candidate.begin(), candidate.end()
    );
    return mismatch.first == root.end();
}

std::optional<PowerMenuCanvas> read_canvas(
    const Json& asset_json,
    std::string* error_message
) {
    try {
        const auto& value = asset_json.at("canvas").at("1x");
        PowerMenuCanvas canvas{
            .width = value.at(0).get<int>(),
            .height = value.at(1).get<int>(),
        };
        if (canvas.width <= 0 || canvas.height <= 0) {
            set_error(error_message, "Power-menu asset has an invalid 1x canvas");
            return std::nullopt;
        }
        return canvas;
    } catch (const std::exception& error) {
        set_error(
            error_message,
            "Invalid power-menu 1x canvas: " + std::string(error.what())
        );
        return std::nullopt;
    }
}

std::optional<PowerMenuPlacement> read_placement(
    const Json& asset_json,
    std::string* error_message
) {
    try {
        const auto& value = asset_json.at("placement").at("1x");
        PowerMenuPlacement placement{
            .x = value.at("x").get<int>(),
            .y = value.at("y").get<int>(),
            .width = value.at("width").get<int>(),
            .height = value.at("height").get<int>(),
        };
        if (placement.width <= 0 || placement.height <= 0) {
            set_error(error_message, "Power-menu asset has an invalid 1x placement size");
            return std::nullopt;
        }
        return placement;
    } catch (const std::exception& error) {
        set_error(
            error_message,
            "Invalid power-menu 1x placement: " + std::string(error.what())
        );
        return std::nullopt;
    }
}

} // namespace

std::optional<PowerMenuManifest> PowerMenuManifest::load(
    const std::filesystem::path& manifest_path,
    std::string* error_message
) {
    std::error_code error;
    const auto canonical_manifest = std::filesystem::weakly_canonical(
        manifest_path,
        error
    );
    if (error || !std::filesystem::is_regular_file(canonical_manifest, error) || error) {
        set_error(
            error_message,
            "Power-menu manifest does not exist: " + manifest_path.string()
        );
        return std::nullopt;
    }

    const auto manifest_json = read_json(canonical_manifest, error_message);
    if (!manifest_json) return std::nullopt;

    PowerMenuManifest manifest;
    manifest.root = canonical_manifest.parent_path();
    std::unordered_set<std::string> asset_names;

    try {
        const auto& assets_json = manifest_json->at("assets");
        if (!assets_json.is_array() || assets_json.empty()) {
            set_error(error_message, "Power-menu manifest contains no assets");
            return std::nullopt;
        }

        manifest.assets.reserve(assets_json.size());
        for (const auto& asset_json : assets_json) {
            PowerMenuAsset asset;
            asset.name = asset_json.at("name").get<std::string>();
            asset.category = asset_json.at("category").get<std::string>();
            asset.file_1x = asset_json.at("files").at("1x").get<std::string>();

            if (asset.name.empty() || !asset_names.insert(asset.name).second) {
                set_error(
                    error_message,
                    "Power-menu asset name is empty or duplicated: " + asset.name
                );
                return std::nullopt;
            }
            if (asset.category.empty() || asset.file_1x.empty()) {
                set_error(
                    error_message,
                    "Power-menu asset '" + asset.name + "' has an empty category or 1x file"
                );
                return std::nullopt;
            }

            const std::filesystem::path relative_path(asset.file_1x);
            if (relative_path.is_absolute()) {
                set_error(
                    error_message,
                    "Power-menu asset uses an absolute path: " + asset.file_1x
                );
                return std::nullopt;
            }

            error.clear();
            const auto canonical_asset = std::filesystem::weakly_canonical(
                manifest.root / relative_path,
                error
            );
            if (error || !path_is_inside(manifest.root, canonical_asset) ||
                !std::filesystem::is_regular_file(canonical_asset, error) || error) {
                set_error(
                    error_message,
                    "Power-menu asset file is missing or escapes its root: " +
                        asset.file_1x
                );
                return std::nullopt;
            }
            asset.path_1x = canonical_asset;

            const auto canvas = read_canvas(asset_json, error_message);
            const auto placement = read_placement(asset_json, error_message);
            if (!canvas || !placement) return std::nullopt;
            asset.canvas_1x = *canvas;
            asset.placement_1x = *placement;

            manifest.assets.push_back(std::move(asset));
        }
    } catch (const std::exception& exception) {
        set_error(
            error_message,
            "Invalid power-menu manifest structure: " + std::string(exception.what())
        );
        return std::nullopt;
    }

    const PowerMenuAsset* static_base = manifest.find_asset("static-base");
    if (static_base == nullptr || static_base->placement_1x.x != 0 ||
        static_base->placement_1x.y != 0 ||
        static_base->placement_1x.width != static_base->canvas_1x.width ||
        static_base->placement_1x.height != static_base->canvas_1x.height) {
        set_error(
            error_message,
            "Power-menu manifest requires a full-canvas static-base asset"
        );
        return std::nullopt;
    }
    manifest.logical_canvas = static_base->canvas_1x;

    for (const auto& asset : manifest.assets) {
        if (asset.canvas_1x.width != manifest.logical_canvas.width ||
            asset.canvas_1x.height != manifest.logical_canvas.height) {
            set_error(
                error_message,
                "Power-menu asset '" + asset.name + "' disagrees with the logical canvas"
            );
            return std::nullopt;
        }
    }

    return manifest;
}

const PowerMenuAsset* PowerMenuManifest::find_asset(std::string_view name) const {
    const auto found = std::find_if(
        assets.begin(),
        assets.end(),
        [name](const PowerMenuAsset& asset) { return asset.name == name; }
    );
    return found == assets.end() ? nullptr : &*found;
}

} // namespace realmheart::ui::powermenu
