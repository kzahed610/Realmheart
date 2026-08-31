#pragma once

#include "core/DisplayTier.hpp"

#include <string>
#include <string_view>

namespace realmheart::ui::workspace {

[[nodiscard]] std::string workspace_overview_asset_path(
    std::string_view family,
    std::string_view filename,
    core::DisplayTier display_tier
);

} // namespace realmheart::ui::workspace
