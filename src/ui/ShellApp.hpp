#pragma once

#include "ui/wallpaper/WallpaperBackend.hpp"

namespace realmheart::ui {

int run_shell(
    wallpaper::WallpaperBackendType wallpaper_backend,
    int restart_readiness_fd = -1
);
int run_shell_lifetime_stress(
    wallpaper::WallpaperBackendType wallpaper_backend,
    int iterations
);

} // namespace realmheart::ui
