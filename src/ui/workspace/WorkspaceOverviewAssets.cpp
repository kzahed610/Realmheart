#include "ui/workspace/WorkspaceOverviewAssets.hpp"

namespace realmheart::ui::workspace {

std::string workspace_overview_asset_path(
    std::string_view family,
    std::string_view filename,
    core::DisplayTier display_tier
) {
    std::string path = "workspace-overview/";
    path.append(family);
    path.push_back('/');
    path.append(core::display_tier_directory(display_tier));
    path.push_back('/');
    path.append(filename);
    return path;
}

} // namespace realmheart::ui::workspace
