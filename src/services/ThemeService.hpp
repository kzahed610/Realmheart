#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <memory>

namespace realmheart::services {

struct Palette {
    std::unordered_map<std::string, std::string> colors;

    std::string get(const std::string& key, const std::string& fallback = "#000000") const {
        auto it = colors.find(key);
        return (it != colors.end()) ? it->second : fallback;
    }
};

class ThemeService {
public:
    ThemeService();
    ~ThemeService() = default;

    ThemeService(const ThemeService&) = delete;
    ThemeService& operator=(const ThemeService&) = delete;

    void update_palette(Palette new_palette);
    Palette get_palette() const { return m_palette; }
    const Palette& active_palette() const { return m_palette; }

    // Callback system for UI components to react to theme changes
    using ThemeChangedCallback = std::function<void(const Palette&)>;
    void subscribe(ThemeChangedCallback callback);

private:
    Palette m_palette;
    std::vector<ThemeChangedCallback> m_subscribers;
};

} // namespace realmheart::services
