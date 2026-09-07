#pragma once

#include "services/WallpaperSource.hpp"

#include <functional>
#include <optional>
#include <string>

namespace realmheart::ui::wallpaper {

// Coordinates a visual wallpaper apply with its durable state update. A
// transaction reports success only after persistence succeeds. If persistence
// fails, the previous visual wallpaper is restored when a rollback path and
// previous path are available; the completion still reports failure because
// no durable success was established.
class WallpaperTransaction final {
public:
    using Completion = std::function<void(bool, std::string)>;
    using VisualApply = std::function<void(
        const realmheart::services::WallpaperSource&,
        Completion
    )>;
    using Persist = std::function<bool(
        const realmheart::services::WallpaperSource&,
        std::string*
    )>;

    struct Request {
        realmheart::services::WallpaperSource desired_source;
        std::optional<realmheart::services::WallpaperSource> previous_source;
        VisualApply apply_visual;
        VisualApply rollback_visual;
        Persist persist;
        Completion completion;
    };

    static void run(Request request);
};

} // namespace realmheart::ui::wallpaper
