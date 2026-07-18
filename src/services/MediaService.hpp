#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

struct _GDBusConnection;
using GDBusConnection = _GDBusConnection;

namespace realmheart::services {

struct MediaInfo {
    std::string title;
    std::string artist;
    std::string album;
    std::string art_url;
    std::string player_bus_name;
    std::string track_id;
    std::int64_t position_us = 0;
    std::int64_t length_us = 0;
    bool can_seek = false;
    // Playback status: 0 = stopped, 1 = playing, 2 = paused
    int playback_status = 0;
};

class MediaService {
private:
    struct SubscriberRegistry;

public:
    using ChangedCallback = std::function<void()>;

    class Subscription {
    public:
        Subscription() = default;
        ~Subscription();
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;
        void reset();

    private:
        friend class MediaService;
        Subscription(std::weak_ptr<SubscriberRegistry> registry, std::size_t id)
            : registry_(std::move(registry)), id_(id) {}
        std::weak_ptr<SubscriberRegistry> registry_;
        std::size_t id_ = 0;
    };

    MediaService() = default;
    ~MediaService();

    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;

    // Reads MPRIS directly over the session bus; callers may safely invoke this
    // from a worker thread without spawning playerctl processes.
    std::optional<MediaInfo> get_current_media();

    bool play_pause();
    bool next();
    bool previous();
    bool seek_to(
        std::string player_bus_name,
        std::string track_id,
        std::int64_t current_position_us,
        std::int64_t target_position_us
    );

    // Relevant MPRIS PropertiesChanged events and NameOwnerChanged wake
    // subscribers immediately. A caller may still keep a slow fallback poll
    // for bus reconnect edge cases.
    Subscription subscribe(ChangedCallback callback);

private:
    struct SubscriberRegistry {
        std::mutex mutex;
        std::size_t next_id = 1;
        std::unordered_map<std::size_t, ChangedCallback> callbacks;
    };

    bool call_mpris_method(const std::string& method);
    std::optional<std::string> current_player_name();
    bool ensure_signal_monitor();
    void notify_changed();
    void clear_cached_player();

    std::mutex mutex_;
    std::string last_player_;
    GDBusConnection* signal_connection_ = nullptr;
    unsigned int properties_subscription_id_ = 0;
    unsigned int names_subscription_id_ = 0;
    std::shared_ptr<SubscriberRegistry> subscribers_ = std::make_shared<SubscriberRegistry>();
};

} // namespace realmheart::services
