#pragma once

#include "wallpaper-native/source/RuntimeWallpaperSource.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace realmheart::wallpaper_native {

// Keeps one source per wallpaper identity so the same wallpaper feeding several
// outputs shares one decode timeline, one frame queue, and one presentation
// clock (renderer docs section 11).
//
// Entries are weak references, which is what makes the sharing fall out for
// free: the renderer's slots hold the strong references, so when the last slot
// release goes away the source stops its own worker and dies. A later acquire
// for the same path then creates a fresh source rather than resurrecting one
// whose decoder was torn down.
class RuntimeSourceRegistry {
public:
    // Creates the source when the identity is not already live.
    using Factory = std::function<RuntimeSourcePtr()>;

    // Identifies a source by canonical path plus the file's device/inode, so a
    // file replaced at the same path is not mistaken for the one already open.
    [[nodiscard]] static std::string identity_for(
        const std::string& path,
        std::string* reason = nullptr
    );

    // Returns the live source for the path's identity, or creates one with
    // 'create' (only called on a miss).
    [[nodiscard]] RuntimeSourcePtr acquire(
        const std::string& path,
        Factory create,
        std::string* error_message = nullptr
    );

    // Drops dead entries. Returns the number of live sources left.
    [[nodiscard]] std::size_t prune();

    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::map<std::string, std::weak_ptr<RuntimeWallpaperSource>> entries_;
};

} // namespace realmheart::wallpaper_native
