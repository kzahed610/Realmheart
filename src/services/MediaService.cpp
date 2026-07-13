#include "services/MediaService.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::services {
namespace {

constexpr const char* kObjectPath = "/org/mpris/MediaPlayer2";
constexpr const char* kPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr int kDbusTimeoutMs = 750;

struct PlayerState {
    std::string bus_name;
    MediaInfo info;
};

std::vector<std::string> list_players(GDBusConnection* connection) {
    std::vector<std::string> players;
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        nullptr,
        G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE,
        kDbusTimeoutMs,
        nullptr,
        &error
    );
    if (reply == nullptr) {
        g_clear_error(&error);
        return players;
    }

    GVariant* names = nullptr;
    g_variant_get(reply, "(@as)", &names);
    GVariantIter iterator;
    const gchar* name = nullptr;
    g_variant_iter_init(&iterator, names);
    while (g_variant_iter_next(&iterator, "&s", &name)) {
        constexpr std::string_view prefix = "org.mpris.MediaPlayer2.";
        if (name != nullptr && std::string_view(name).starts_with(prefix)) {
            players.emplace_back(name);
        }
    }
    g_variant_unref(names);
    g_variant_unref(reply);
    return players;
}

std::string first_artist(GVariant* metadata) {
    GVariant* artists = g_variant_lookup_value(metadata, "xesam:artist", G_VARIANT_TYPE("as"));
    if (artists == nullptr) return {};

    std::string artist;
    GVariantIter iterator;
    const gchar* value = nullptr;
    g_variant_iter_init(&iterator, artists);
    if (g_variant_iter_next(&iterator, "&s", &value) && value != nullptr) artist = value;
    g_variant_unref(artists);
    return artist;
}

std::optional<PlayerState> read_player(GDBusConnection* connection, const std::string& bus_name) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        bus_name.c_str(),
        kObjectPath,
        "org.freedesktop.DBus.Properties",
        "GetAll",
        g_variant_new("(s)", kPlayerInterface),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        kDbusTimeoutMs,
        nullptr,
        &error
    );
    if (reply == nullptr) {
        g_clear_error(&error);
        return std::nullopt;
    }

    GVariant* properties = nullptr;
    g_variant_get(reply, "(@a{sv})", &properties);

    PlayerState state;
    state.bus_name = bus_name;
    const gchar* playback = nullptr;
    if (g_variant_lookup(properties, "PlaybackStatus", "&s", &playback) && playback != nullptr) {
        if (std::string_view(playback) == "Playing") state.info.playback_status = 1;
        else if (std::string_view(playback) == "Paused") state.info.playback_status = 2;
    }

    GVariant* metadata = g_variant_lookup_value(properties, "Metadata", G_VARIANT_TYPE("a{sv}"));
    if (metadata != nullptr) {
        const gchar* title = nullptr;
        const gchar* album = nullptr;
        if (g_variant_lookup(metadata, "xesam:title", "&s", &title) && title != nullptr) {
            state.info.title = title;
        }
        if (g_variant_lookup(metadata, "xesam:album", "&s", &album) && album != nullptr) {
            state.info.album = album;
        }
        state.info.artist = first_artist(metadata);
        g_variant_unref(metadata);
    }

    g_variant_unref(properties);
    g_variant_unref(reply);
    return state;
}

std::optional<PlayerState> select_player(GDBusConnection* connection) {
    std::optional<PlayerState> fallback;
    for (const auto& name : list_players(connection)) {
        auto player = read_player(connection, name);
        if (!player) continue;
        if (player->info.playback_status == 1) return player;
        if (!fallback || (fallback->info.playback_status == 0 && player->info.playback_status == 2)) {
            fallback = std::move(player);
        }
    }
    return fallback;
}

} // namespace

MediaService::Subscription::~Subscription() { reset(); }
MediaService::Subscription::Subscription(Subscription&& other) noexcept
    : registry_(std::move(other.registry_)), id_(std::exchange(other.id_, 0)) {}
MediaService::Subscription& MediaService::Subscription::operator=(Subscription&& other) noexcept {
    if (this == &other) return *this;
    reset();
    registry_ = std::move(other.registry_);
    id_ = std::exchange(other.id_, 0);
    return *this;
}
void MediaService::Subscription::reset() {
    if (id_ == 0) return;
    if (const auto registry = registry_.lock()) {
        std::lock_guard lock(registry->mutex);
        registry->callbacks.erase(id_);
    }
    registry_.reset();
    id_ = 0;
}

MediaService::~MediaService() {
    if (signal_connection_ != nullptr) {
        if (properties_subscription_id_ != 0) {
            g_dbus_connection_signal_unsubscribe(signal_connection_, properties_subscription_id_);
        }
        if (names_subscription_id_ != 0) {
            g_dbus_connection_signal_unsubscribe(signal_connection_, names_subscription_id_);
        }
        g_object_unref(signal_connection_);
        signal_connection_ = nullptr;
    }
}

bool MediaService::ensure_signal_monitor() {
    if (signal_connection_ != nullptr && properties_subscription_id_ != 0 && names_subscription_id_ != 0) {
        return true;
    }
    if (signal_connection_ != nullptr) {
        if (properties_subscription_id_ != 0) {
            g_dbus_connection_signal_unsubscribe(signal_connection_, properties_subscription_id_);
        }
        if (names_subscription_id_ != 0) {
            g_dbus_connection_signal_unsubscribe(signal_connection_, names_subscription_id_);
        }
        g_object_unref(signal_connection_);
        signal_connection_ = nullptr;
        properties_subscription_id_ = 0;
        names_subscription_id_ = 0;
    }

    GError* error = nullptr;
    signal_connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (signal_connection_ == nullptr) {
        g_clear_error(&error);
        return false;
    }

    properties_subscription_id_ = g_dbus_connection_signal_subscribe(
        signal_connection_,
        nullptr,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        kObjectPath,
        kPlayerInterface,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
            auto* self = static_cast<MediaService*>(data);
            // Playback changes can make a different player the best target.
            // Drop the cached bus name before waking UI/control callers.
            self->clear_cached_player();
            self->notify_changed();
        },
        this,
        nullptr
    );
    names_subscription_id_ = g_dbus_connection_signal_subscribe(
        signal_connection_,
        "org.freedesktop.DBus",
        "org.freedesktop.DBus",
        "NameOwnerChanged",
        "/org/freedesktop/DBus",
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* parameters, gpointer data) {
            const gchar* name = nullptr;
            const gchar* old_owner = nullptr;
            const gchar* new_owner = nullptr;
            g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
            static_cast<void>(old_owner);
            static_cast<void>(new_owner);
            constexpr std::string_view prefix = "org.mpris.MediaPlayer2.";
            if (name != nullptr && std::string_view(name).starts_with(prefix)) {
                auto* self = static_cast<MediaService*>(data);
                self->clear_cached_player();
                self->notify_changed();
            }
        },
        this,
        nullptr
    );
    if (properties_subscription_id_ != 0 && names_subscription_id_ != 0) return true;

    if (properties_subscription_id_ != 0) {
        g_dbus_connection_signal_unsubscribe(signal_connection_, properties_subscription_id_);
    }
    if (names_subscription_id_ != 0) {
        g_dbus_connection_signal_unsubscribe(signal_connection_, names_subscription_id_);
    }
    g_object_unref(signal_connection_);
    signal_connection_ = nullptr;
    properties_subscription_id_ = 0;
    names_subscription_id_ = 0;
    return false;
}

MediaService::Subscription MediaService::subscribe(ChangedCallback callback) {
    if (!callback) return {};
    static_cast<void>(ensure_signal_monitor());
    std::lock_guard lock(subscribers_->mutex);
    const std::size_t id = subscribers_->next_id++;
    subscribers_->callbacks.emplace(id, std::move(callback));
    return Subscription{subscribers_, id};
}

void MediaService::notify_changed() {
    std::vector<ChangedCallback> callbacks;
    {
        std::lock_guard lock(subscribers_->mutex);
        callbacks.reserve(subscribers_->callbacks.size());
        for (const auto& [_, callback] : subscribers_->callbacks) callbacks.push_back(callback);
    }
    for (auto& callback : callbacks) {
        if (callback) callback();
    }
}

void MediaService::clear_cached_player() {
    std::lock_guard lock(mutex_);
    last_player_.clear();
}

std::optional<MediaInfo> MediaService::get_current_media() {
    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
        g_clear_error(&error);
        return std::nullopt;
    }

    auto player = select_player(connection);
    g_object_unref(connection);
    if (!player) return std::nullopt;

    {
        std::lock_guard lock(mutex_);
        last_player_ = player->bus_name;
    }
    return player->info;
}

std::optional<std::string> MediaService::current_player_name() {
    {
        std::lock_guard lock(mutex_);
        if (!last_player_.empty()) return last_player_;
    }

    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
        g_clear_error(&error);
        return std::nullopt;
    }
    auto selected = select_player(connection);
    g_object_unref(connection);
    if (!selected) return std::nullopt;

    std::lock_guard lock(mutex_);
    last_player_ = selected->bus_name;
    return last_player_;
}

bool MediaService::call_mpris_method(const std::string& method) {
    const auto player = current_player_name();
    if (!player) return false;

    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
        g_clear_error(&error);
        return false;
    }

    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        player->c_str(),
        kObjectPath,
        kPlayerInterface,
        method.c_str(),
        nullptr,
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        kDbusTimeoutMs,
        nullptr,
        &error
    );
    const bool success = reply != nullptr;
    if (reply != nullptr) g_variant_unref(reply);
    g_clear_error(&error);
    g_object_unref(connection);
    if (!success) clear_cached_player();
    return success;
}

bool MediaService::play_pause() { return call_mpris_method("PlayPause"); }
bool MediaService::next() { return call_mpris_method("Next"); }
bool MediaService::previous() { return call_mpris_method("Previous"); }

} // namespace realmheart::services
