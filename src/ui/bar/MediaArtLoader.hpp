#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace realmheart::ui::bar {

// Resolves local MPRIS artwork and keeps remote artwork in a small on-disk
// cache. Remote downloads are optional (curl) and only happen when the art URL
// changes; the taskbar never keeps full-resolution decoded artwork in memory.
class MediaArtLoader {
public:
    static std::optional<std::filesystem::path> resolve(
        std::string_view art_url,
        const std::function<bool()>& cancelled = {}
    );
};

} // namespace realmheart::ui::bar
