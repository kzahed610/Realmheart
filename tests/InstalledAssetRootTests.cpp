#include "animation/character/CharacterManifest.hpp"
#include "core/DisplayTier.hpp"
#include "ui/AssetResolver.hpp"
#include "ui/workspace/WorkspaceOverviewAssets.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
namespace fs = std::filesystem;

struct TierCase {
    realmheart::core::DisplayTier tier;
    std::string_view directory;
};

constexpr std::array<TierCase, 3> kTiers{{
    {realmheart::core::DisplayTier::P1080, "1080p"},
    {realmheart::core::DisplayTier::P1440, "1440p"},
    {realmheart::core::DisplayTier::P4K, "4k"},
}};

constexpr std::array<std::string_view, 4> kBackgrounds{{
    "fire-the-hearth.png",
    "water-etistin-bay.png",
    "wind-elshire-forest.png",
    "earth-vildorial.png",
}};

constexpr std::array<std::string_view, 4> kCharacters{{
    "bairon-wykes.png",
    "varay-aurae.png",
    "aya-grephin.png",
    "mica-earthborn.png",
}};

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool path_is_inside(const fs::path& root, const fs::path& candidate) {
    std::error_code error;
    const auto canonical_root = fs::weakly_canonical(root, error);
    if (error) return false;
    const auto canonical_candidate = fs::weakly_canonical(candidate, error);
    if (error) return false;
    const auto mismatch = std::mismatch(
        canonical_root.begin(), canonical_root.end(),
        canonical_candidate.begin(), canonical_candidate.end()
    );
    return mismatch.first == canonical_root.end();
}

fs::path require_installed_asset(const fs::path& asset_root, const std::string& relative) {
    const auto resolved = realmheart::ui::resolve_project_asset(relative);
    require(resolved.has_value(), "installed asset did not resolve: " + relative);
    require(path_is_inside(asset_root, *resolved),
            "resolver escaped staged installed asset root: " + resolved->string());
    return *resolved;
}

void verify_workspace_assets(const fs::path& asset_root) {
    for (const auto& tier : kTiers) {
        for (const auto filename : kBackgrounds) {
            require_installed_asset(
                asset_root,
                realmheart::ui::workspace::workspace_overview_asset_path(
                    "backgrounds", filename, tier.tier
                )
            );
        }
        for (const auto filename : kCharacters) {
            require_installed_asset(
                asset_root,
                realmheart::ui::workspace::workspace_overview_asset_path(
                    "characters", filename, tier.tier
                )
            );
        }
    }
}

void verify_tessia_packages(const fs::path& asset_root) {
    const auto rig = require_installed_asset(asset_root, "characters/tessia/rig.json");
    const auto character_root = rig.parent_path();

    for (const auto& tier : kTiers) {
        std::string manifest_relative = "characters/tessia/";
        manifest_relative.append(tier.directory);
        manifest_relative.append("/manifest.json");
        require_installed_asset(asset_root, manifest_relative);

        std::string error;
        const auto manifest = realmheart::animation::character::CharacterManifest::load(
            character_root, tier.tier, &error
        );
        require(manifest.has_value(),
                "installed Tessia package failed to load for " + std::string(tier.directory) +
                    ": " + error);
        require(manifest->display_tier == tier.tier,
                "installed Tessia package selected the wrong tier");
        require(path_is_inside(asset_root, manifest->root),
                "installed Tessia manifest root escaped staged asset tree");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || argv[1] == nullptr || *argv[1] == '\0') {
        std::cerr << "Usage: realmheart_installed_asset_root_probe <staged-asset-root>\n";
        return 2;
    }

    const fs::path asset_root = argv[1];
    require(fs::is_directory(asset_root),
            "staged installed asset root does not exist: " + asset_root.string());
    verify_workspace_assets(asset_root);
    verify_tessia_packages(asset_root);
    std::cout << "Installed asset root tests passed: " << asset_root << '\n';
    return 0;
}
