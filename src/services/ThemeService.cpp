#include "services/ThemeService.hpp"
#include <iostream>

namespace realmheart::services {

ThemeService::ThemeService() {
    // Default fallback palette (Catppuccin Mocha approximation)
    m_palette.colors = {
        {"primary", "#cba6f7"},
        {"secondary", "#89b4fa"},
        {"tertiary", "#f5c2e7"},
        {"background", "#11111b"},
        {"surface", "#1e1e2e"},
        {"text", "#cdd6f4"},
        {"outline", "#45475a"}
    };
}

void ThemeService::update_palette(Palette new_palette) {
    m_palette = std::move(new_palette);
    std::cout << "[ThemeService] Palette updated. Primary color: " << m_palette.get("primary") << std::endl;
    for (auto& callback : m_subscribers) {
        callback(m_palette);
    }
}

void ThemeService::subscribe(ThemeChangedCallback callback) {
    m_subscribers.push_back(std::move(callback));
}

} // namespace realmheart::services
