#include "ui/wallpaper/WallpaperStartupPlan.hpp"

namespace realmheart::ui::wallpaper {

WallpaperStartupPlan make_startup_wallpaper_plan(
    const std::vector<WallpaperStartupOutput>& outputs,
    const std::optional<WallpaperSource>& global_fallback
) {
    WallpaperStartupPlan plan;
    plan.jobs.reserve(outputs.size());

    for (const auto& output : outputs) {
        if (!output.target.valid()) continue;

        const WallpaperSource* source = nullptr;
        if (output.output_source && !output.output_source->empty()) {
            source = &*output.output_source;
            plan.has_output_overrides = true;
        } else if (global_fallback && !global_fallback->empty()) {
            source = &*global_fallback;
        }
        if (source == nullptr) continue;

        plan.jobs.push_back({output.target, *source});
    }

    return plan;
}

} // namespace realmheart::ui::wallpaper
