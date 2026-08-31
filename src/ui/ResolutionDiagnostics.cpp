#include "ui/ResolutionDiagnostics.hpp"

#include "core/DisplayTier.hpp"
#include "ui/AssetResolver.hpp"
#include "ui/NotesGeometry.hpp"
#include "ui/bar/BarGeometry.hpp"
#include "ui/launcher/LauncherGeometry.hpp"
#include "ui/sidebar/SidebarGeometry.hpp"
#include "ui/workspace/WorkspaceOverviewAssets.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace realmheart::ui {
namespace {

struct TierCase {
    core::DisplayTier tier;
    std::string_view label;
};

constexpr std::array<TierCase, 3> kTiers{{
    {core::DisplayTier::P1080, "1080p"},
    {core::DisplayTier::P1440, "1440p"},
    {core::DisplayTier::P4K, "4k"},
}};

void append_asset_line(
    std::ostringstream& report,
    std::string_view label,
    const std::string& relative_path
) {
    report << "    " << label << ": " << relative_path;
    const auto resolved = resolve_project_asset(relative_path);
    if (resolved) {
        report << " -> " << resolved->string();
    } else {
        report << " -> [missing]";
    }
    report << '\n';
}

} // namespace

std::string resolution_compatibility_report() {
    std::ostringstream report;
    report << "Realmheart resolution compatibility report\n"
           << "=========================================\n"
           << "Tier selection is based on the assigned monitor's logical geometry.\n"
           << "GTK layout values below are logical pixels; they are not multiplied by GDK scale.\n\n";

    for (const auto& tier_case : kTiers) {
        const auto spec = core::display_tier_spec(tier_case.tier);
        const auto bar = bar::bar_geometry_for_display_tier(tier_case.tier);
        const auto sidebar = sidebar::SidebarFrameLayout::for_display_tier(tier_case.tier);
        const auto launcher = launcher::launcher_geometry_for_display_tier(tier_case.tier);
        const auto notes = notes_layout_for_display_tier(tier_case.tier);

        report << tier_case.label << " (" << spec.logical_width << 'x'
               << spec.logical_height << ")\n"
               << "  taskbar: rail=" << bar.rail_width
               << " cap=" << bar.cap_extension
               << " visual=" << bar.visual_width
               << " icon=" << bar.icon_size
               << " launcher=" << bar.launcher_icon_size << '\n'
               << "  sidebar: frame=" << sidebar.frame_width
               << " character-host=" << sidebar.character_gutter_width
               << " content=" << sidebar.content_width()
               << " surface=" << sidebar.surface_width()
               << " hotspot=" << sidebar.hotspot_hit_width << '\n'
               << "  launcher: centre=" << launcher.centre_final_width << 'x'
               << launcher.centre_height << " search=" << launcher.search_final_width
               << " node=" << launcher.constellation_node_width << 'x'
               << launcher.constellation_node_height << '\n'
               << "  notes: " << notes.window_width << 'x' << notes.window_height
               << " margins=" << notes.text_margin_horizontal << '/'
               << notes.text_margin_top << '/' << notes.text_margin_bottom << '\n'
               << "  asset provenance:\n";

        append_asset_line(
            report,
            "workspace background",
            workspace::workspace_overview_asset_path(
                "backgrounds", "fire-the-hearth.png", tier_case.tier
            )
        );
        append_asset_line(
            report,
            "workspace character",
            workspace::workspace_overview_asset_path(
                "characters", "bairon-wykes.png", tier_case.tier
            )
        );

        std::string tessia_manifest = "characters/tessia/";
        tessia_manifest.append(core::display_tier_directory(tier_case.tier));
        tessia_manifest.append("/manifest.json");
        append_asset_line(report, "Tessia manifest", tessia_manifest);
        report << '\n';
    }

    return report.str();
}

} // namespace realmheart::ui
