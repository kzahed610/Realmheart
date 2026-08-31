#include "core/DisplayTier.hpp"
#include "ui/workspace/WorkspaceOverviewAssets.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_paths_use_selected_display_tier() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::workspace::workspace_overview_asset_path;

    require(
        workspace_overview_asset_path(
            "backgrounds", "fire-the-hearth.png", DisplayTier::P1080
        ) == "workspace-overview/backgrounds/1080p/fire-the-hearth.png",
        "1080p background path must use the selected tier"
    );
    require(
        workspace_overview_asset_path(
            "characters", "varay-aurae.png", DisplayTier::P1440
        ) == "workspace-overview/characters/1440p/varay-aurae.png",
        "1440p character path must use the selected tier"
    );
    require(
        workspace_overview_asset_path(
            "backgrounds", "earth-vildorial.png", DisplayTier::P4K
        ) == "workspace-overview/backgrounds/4k/earth-vildorial.png",
        "4K background path must use the selected tier"
    );
}

} // namespace

int main() {
    test_paths_use_selected_display_tier();
    std::cout << "Workspace overview asset path tests passed\n";
    return 0;
}
