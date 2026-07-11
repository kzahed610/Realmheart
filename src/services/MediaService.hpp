#pragma once

#include <string>
#include <optional>
#include <vector>

namespace realmheart::services {

struct MediaInfo {
    std::string title;
    std::string artist;
    std::string album;
    // Playback status: 0 = stopped, 1 = playing, 2 = paused
    int playback_status; 
};

class MediaService {
public:
    MediaService() = default;
    ~MediaService() = default;

    // Fetch current media info from MPRIS players via DBus
    std::optional<MediaInfo> get_current_media();

    // Control media (these typically send DBus messages)
    bool play_pause();
    bool next();
    bool previous();

private:
    // Helper to call a DBus method on a specific player
    bool call_mpris_method(const std::string& player, const std::string& interface, const std::string& method);
};

} // namespace realmheart::services
