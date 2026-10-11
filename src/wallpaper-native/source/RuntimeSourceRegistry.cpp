#include "wallpaper-native/source/RuntimeSourceRegistry.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace realmheart::wallpaper_native {

std::string RuntimeSourceRegistry::identity_for(
    const std::string& path,
    std::string* reason
) {
    if (reason != nullptr) reason->clear();

    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) canonical = std::filesystem::path(path).lexically_normal();
    std::string identity = canonical.generic_string();

    // The stat identity is what keeps a file replaced at the same path from
    // reusing the source that already opened the previous one.
    struct stat metadata {};
    if (::stat(path.c_str(), &metadata) == 0) {
        identity += "\n";
        identity += std::to_string(static_cast<unsigned long long>(metadata.st_dev));
        identity += ":";
        identity += std::to_string(static_cast<unsigned long long>(metadata.st_ino));
    } else if (reason != nullptr) {
        *reason = "wallpaper path could not be identified";
    }
    return identity;
}

RuntimeSourcePtr RuntimeSourceRegistry::acquire(
    const std::string& path,
    Factory create,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    const std::string identity = identity_for(path, error_message);

    const auto found = entries_.find(identity);
    if (found != entries_.end()) {
        if (const RuntimeSourcePtr live = found->second.lock()) {
            // One source, several consumers.
            return live;
        }
        entries_.erase(found);
    }

    RuntimeSourcePtr source = create != nullptr ? create() : nullptr;
    if (source == nullptr) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "wallpaper source could not be created";
        }
        return nullptr;
    }
    entries_.insert_or_assign(identity, source);
    return source;
}

std::size_t RuntimeSourceRegistry::prune() {
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        if (entry->second.expired()) {
            entry = entries_.erase(entry);
        } else {
            ++entry;
        }
    }
    return entries_.size();
}

std::size_t RuntimeSourceRegistry::size() const noexcept {
    std::size_t live = 0;
    for (const auto& entry : entries_) {
        if (!entry.second.expired()) ++live;
    }
    return live;
}

} // namespace realmheart::wallpaper_native
