#include "services/MediaService.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <cstdio>

namespace realmheart::services {

// Helper to execute shell command and get output
std::string exec(const char* cmd) {
    char buffer[128];
    std::string result = "";
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "ERROR";
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        result += buffer;
    }
    pclose(pipe);
    // Trim trailing newline
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

std::optional<MediaInfo> MediaService::get_current_media() {
    // We use playerctl as the standard CLI wrapper for MPRIS
    // playerctl metadata --format "{{ artist }} - {{ title }}"
    std::string info = exec("playerctl metadata --format '{{ artist }} | {{ title }} | {{ album }}' 2>/dev/null");
    
    if (info == "ERROR" || info.empty()) {
        return std::nullopt;
    }

    // Split the string by '|'
    std::vector<std::string> parts;
    std::stringstream ss(info);
    std::string segment;
    while (std::getline(ss, segment, '|')) {
        parts.push_back(segment);
    }

    if (parts.size() < 2) return std::nullopt;

    MediaInfo media;
    media.artist = parts[0];
    media.title = parts[1];
    media.album = (parts.size() > 2) ? parts[2] : "";
    
    // Get playback status
    std::string status = exec("playerctl status 2>/dev/null");
    if (status == "Playing") media.playback_status = 1;
    else if (status == "Paused") media.playback_status = 2;
    else media.playback_status = 0;

    return media;
}

bool MediaService::play_pause() {
    return system("playerctl play-pause > /dev/null 2>&1") == 0;
}

bool MediaService::next() {
    return system("playerctl next > /dev/null 2>&1") == 0;
}

bool MediaService::previous() {
    return system("playerctl previous > /dev/null 2>&1") == 0;
}

} // namespace realmheart::services
