#pragma once

#include "ui/wallpaper/WallpaperBackend.hpp"

#include <optional>
#include <vector>

namespace realmheart::ui::wallpaper {

struct WallpaperStartupOutput {
    WallpaperOutputTarget target;
    std::optional<WallpaperSource> output_source;
};

struct WallpaperStartupJob {
    WallpaperOutputTarget target;
    WallpaperSource source;
};

struct WallpaperStartupPlan {
    bool has_output_overrides = false;
    std::vector<WallpaperStartupJob> jobs;
};

[[nodiscard]] WallpaperStartupPlan make_startup_wallpaper_plan(
    const std::vector<WallpaperStartupOutput>& outputs,
    const std::optional<WallpaperSource>& global_fallback
);

} // namespace realmheart::ui::wallpaper
