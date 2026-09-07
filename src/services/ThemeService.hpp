#pragma once

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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

// Matugen values are eventually embedded in GTK CSS. Keep the trust boundary
// shared by the parser, cache, service, and CSS builder.
[[nodiscard]] bool is_valid_palette_color(std::string_view value);
[[nodiscard]] bool palette_is_valid(const Palette& palette);

class ThemeService {
private:
    struct SubscriberRegistry;

public:
    using ThemeChangedCallback = std::function<void(const Palette&)>;

    enum class PersistenceStatus {
        Idle,
        Pending,
        Committed,
        Failed
    };

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

    explicit ThemeService(std::filesystem::path cache_path = {});
    ~ThemeService();

    ThemeService(const ThemeService&) = delete;
    ThemeService& operator=(const ThemeService&) = delete;

    // Publishes immediately on the caller's thread and schedules cache I/O on
    // the service worker. Invalid palettes are rejected without notification.
    bool update_palette(Palette new_palette);
    [[nodiscard]] Palette get_palette() const;
    [[nodiscard]] Subscription subscribe(ThemeChangedCallback callback);
    [[nodiscard]] PersistenceStatus persistence_status() const;
    [[nodiscard]] bool retry_persistence();
    void wait_for_persistence();
    void ensure_safe_palette();

private:
    struct PendingPersistence {
        Palette palette;
        std::uint64_t generation = 0;
    };

    struct SubscriberRegistry {
        std::mutex mutex;
        std::size_t next_id = 1;
        std::unordered_map<std::size_t, ThemeChangedCallback> callbacks;
    };

    mutable std::mutex palette_mutex_;
    Palette palette_;
    std::filesystem::path cache_path_;
    std::shared_ptr<SubscriberRegistry> subscribers_ = std::make_shared<SubscriberRegistry>();

    mutable std::mutex persistence_mutex_;
    std::condition_variable persistence_cv_;
    std::condition_variable persistence_idle_cv_;
    std::optional<PendingPersistence> pending_persistence_;
    std::thread persistence_worker_;
    std::uint64_t next_persistence_generation_ = 0;
    bool persistence_in_flight_ = false;
    bool stopping_ = false;
    PersistenceStatus persistence_status_ = PersistenceStatus::Idle;

    void persistence_loop();
    void enqueue_persistence(Palette palette);
};

} // namespace realmheart::services
