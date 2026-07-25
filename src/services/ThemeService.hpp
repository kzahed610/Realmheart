#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace realmheart::services {

struct Palette {
    std::unordered_map<std::string, std::string> colors;

    [[nodiscard]] std::string get(
        const std::string& key,
        const std::string& fallback = "#000000"
    ) const {
        const auto it = colors.find(key);
        return it != colors.end() && !it->second.empty() ? it->second : fallback;
    }
};

class ThemeService {
private:
    struct SubscriberRegistry;

public:
    using ThemeChangedCallback = std::function<void(const Palette&)>;

    class Subscription {
    public:
        Subscription() = default;
        ~Subscription();

        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;

        void reset();
        [[nodiscard]] explicit operator bool() const noexcept { return id_ != 0; }

    private:
        friend class ThemeService;
        Subscription(std::weak_ptr<SubscriberRegistry> registry, std::size_t id)
            : registry_(std::move(registry)), id_(id) {}

        std::weak_ptr<SubscriberRegistry> registry_;
        std::size_t id_ = 0;
    };

    ThemeService();
    ~ThemeService() = default;

    ThemeService(const ThemeService&) = delete;
    ThemeService& operator=(const ThemeService&) = delete;

    void update_palette(Palette new_palette);
    [[nodiscard]] Palette get_palette() const;
    [[nodiscard]] Subscription subscribe(ThemeChangedCallback callback);

private:
    struct SubscriberRegistry {
        std::mutex mutex;
        std::size_t next_id = 1;
        std::unordered_map<std::size_t, ThemeChangedCallback> callbacks;
    };

    mutable std::mutex palette_mutex_;
    Palette palette_;
    std::filesystem::path cache_path_;
    std::shared_ptr<SubscriberRegistry> subscribers_ = std::make_shared<SubscriberRegistry>();
};

} // namespace realmheart::services
