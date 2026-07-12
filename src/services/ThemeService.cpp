#include "services/ThemeService.hpp"

#include <iostream>
#include <vector>

namespace realmheart::services {

ThemeService::Subscription::~Subscription() {
    reset();
}

ThemeService::Subscription::Subscription(Subscription&& other) noexcept
    : registry_(std::move(other.registry_)), id_(std::exchange(other.id_, 0)) {}

ThemeService::Subscription& ThemeService::Subscription::operator=(Subscription&& other) noexcept {
    if (this == &other) return *this;
    reset();
    registry_ = std::move(other.registry_);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

void ThemeService::Subscription::reset() {
    if (id_ == 0) return;
    if (const auto registry = registry_.lock()) {
        std::lock_guard lock(registry->mutex);
        registry->callbacks.erase(id_);
    }
    registry_.reset();
    id_ = 0;
}

ThemeService::ThemeService() {
    palette_.colors = {
        {"primary", "#cba6f7"},
        {"accent", "#cba6f7"},
        {"secondary", "#89b4fa"},
        {"tertiary", "#f5c2e7"},
        {"background", "#11111b"},
        {"surface", "#1e1e2e"},
        {"surface_variant", "#313244"},
        {"text", "#cdd6f4"},
        {"text_muted", "#a6adc8"},
        {"outline", "#45475a"},
        {"error", "#f38ba8"},
        {"red", "#f38ba8"},
        {"blue", "#89b4fa"}
    };
}

Palette ThemeService::get_palette() const {
    std::lock_guard lock(palette_mutex_);
    return palette_;
}

void ThemeService::update_palette(Palette new_palette) {
    std::vector<ThemeChangedCallback> callbacks;
    Palette snapshot;

    {
        std::lock_guard lock(palette_mutex_);
        palette_ = std::move(new_palette);
        snapshot = palette_;
    }

    {
        std::lock_guard lock(subscribers_->mutex);
        callbacks.reserve(subscribers_->callbacks.size());
        for (const auto& [_, callback] : subscribers_->callbacks) {
            callbacks.push_back(callback);
        }
    }

    std::cout << "[ThemeService] Palette updated. Primary="
              << snapshot.get("primary")
              << " background=" << snapshot.get("background") << '\n';

    for (auto& callback : callbacks) {
        if (callback) callback(snapshot);
    }
}

ThemeService::Subscription ThemeService::subscribe(ThemeChangedCallback callback) {
    if (!callback) return {};

    std::lock_guard lock(subscribers_->mutex);
    const std::size_t id = subscribers_->next_id++;
    subscribers_->callbacks.emplace(id, std::move(callback));
    return Subscription{subscribers_, id};
}

} // namespace realmheart::services
