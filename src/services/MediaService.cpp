#include "services/MediaService.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::services {
namespace {

constexpr const char* kObjectPath = "/org/mpris/MediaPlayer2";
constexpr const char* kPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr int kDbusTimeoutMs = 750;
constexpr guint kSignalReconnectDelayMs = 1000;
constexpr guint kMaxSignalReconnectDelayMs = 8000;
constexpr unsigned int kMaxSignalReconnectAttempts = 5;
constexpr std::size_t kMaxPlayers = 16;
constexpr auto kDiscoveryBudget = std::chrono::milliseconds(1500);
constexpr std::size_t kMaxTitleBytes = 512;
constexpr std::size_t kMaxArtistBytes = 512;
constexpr std::size_t kMaxAlbumBytes = 512;
constexpr std::size_t kMaxArtUrlBytes = 2048;
constexpr std::size_t kMaxTrackIdBytes = 512;
constexpr std::size_t kMaxBusNameBytes = 256;
constexpr gint64 kMaxMediaDurationUs = 7LL * 24LL * 60LL * 60LL * 1'000'000LL;

std::string bounded_string(const char* value, std::size_t maximum) {
    if (value == nullptr) return {};
    const std::string_view text(value);
    return text.size() <= maximum ? std::string(text) : std::string{};
}

std::optional<gint64> variant_integer(GVariant* value) {
    if (value == nullptr) return std::nullopt;

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant* child = g_variant_get_variant(value);
        const auto result = variant_integer(child);
        g_variant_unref(child);
        return result;
    }
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) {
        return g_variant_get_int64(value);
    }
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT64)) {
        const guint64 raw = g_variant_get_uint64(value);
        return raw > static_cast<guint64>(std::numeric_limits<gint64>::max())
            ? std::numeric_limits<gint64>::max()
            : static_cast<gint64>(raw);
    }
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
        return static_cast<gint64>(g_variant_get_int32(value));
    }
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
        return static_cast<gint64>(g_variant_get_uint32(value));
    }
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE)) {
        const double raw = g_variant_get_double(value);
        if (!std::isfinite(raw)) return std::nullopt;
        return static_cast<gint64>(std::clamp(
            raw,
            static_cast<double>(std::numeric_limits<gint64>::min()),
            static_cast<double>(std::numeric_limits<gint64>::max())
        ));
    }
    return std::nullopt;
}

std::optional<gint64> lookup_integer(GVariant* dictionary, const char* key) {
    if (dictionary == nullptr || key == nullptr) return std::nullopt;
    GVariant* value = g_variant_lookup_value(dictionary, key, nullptr);
    if (value == nullptr) return std::nullopt;
    const auto result = variant_integer(value);
    g_variant_unref(value);
    return result;
}

std::string lookup_string_like(
    GVariant* dictionary,
    const char* key,
    std::size_t maximum
) {
    if (dictionary == nullptr || key == nullptr) return {};
    GVariant* value = g_variant_lookup_value(dictionary, key, nullptr);
    if (value == nullptr) return {};

    while (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant* child = g_variant_get_variant(value);
        g_variant_unref(value);
        value = child;
    }

    std::string result;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ||
        g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) {
        result = bounded_string(g_variant_get_string(value, nullptr), maximum);
    }
    g_variant_unref(value);
    return result;
}

struct PlayerState {
    std::string bus_name;
    MediaInfo info;
};

std::vector<std::string> list_players(
    GDBusConnection* connection,
    std::chrono::steady_clock::time_point deadline
) {
    std::vector<std::string> players;
    GError* error = nullptr;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()
    ).count();
    if (remaining <= 0) return players;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        nullptr,
        G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE,
        static_cast<gint>(std::min<std::int64_t>(kDbusTimeoutMs, remaining)),
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
        if (name != nullptr && std::string_view(name).starts_with(prefix) &&
            std::string_view(name).size() <= kMaxBusNameBytes &&
            players.size() < kMaxPlayers) {
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
    if (g_variant_iter_next(&iterator, "&s", &value) && value != nullptr) {
        artist = bounded_string(value, kMaxArtistBytes);
    }
    g_variant_unref(artists);
    return artist;
}

std::optional<PlayerState> read_player(
    GDBusConnection* connection,
    const std::string& bus_name,
    std::chrono::steady_clock::time_point deadline
) {
    if (bus_name.size() > kMaxBusNameBytes ||
        std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()
    ).count();
    if (remaining <= 0) return std::nullopt;

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
        static_cast<gint>(std::min<std::int64_t>(kDbusTimeoutMs, remaining)),
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
    state.info.player_bus_name = bounded_string(bus_name.c_str(), kMaxBusNameBytes);
    const gchar* playback = nullptr;
    if (g_variant_lookup(properties, "PlaybackStatus", "&s", &playback) && playback != nullptr) {
        if (std::string_view(playback) == "Playing") state.info.playback_status = 1;
        else if (std::string_view(playback) == "Paused") state.info.playback_status = 2;
    }

    gboolean can_seek = FALSE;
    if (g_variant_lookup(properties, "CanSeek", "b", &can_seek)) {
        state.info.can_seek = can_seek;
    }

    if (const auto position_us = lookup_integer(properties, "Position")) {
        state.info.position_us = std::clamp<gint64>(
            *position_us,
            0,
            kMaxMediaDurationUs
        );
    }

    GVariant* metadata = g_variant_lookup_value(properties, "Metadata", G_VARIANT_TYPE("a{sv}"));
    if (metadata != nullptr) {
        const gchar* title = nullptr;
        const gchar* album = nullptr;
        const gchar* art_url = nullptr;
        if (g_variant_lookup(metadata, "xesam:title", "&s", &title) && title != nullptr) {
            state.info.title = bounded_string(title, kMaxTitleBytes);
        }
        if (g_variant_lookup(metadata, "xesam:album", "&s", &album) && album != nullptr) {
            state.info.album = bounded_string(album, kMaxAlbumBytes);
        }
        if (g_variant_lookup(metadata, "mpris:artUrl", "&s", &art_url) && art_url != nullptr) {
            state.info.art_url = bounded_string(art_url, kMaxArtUrlBytes);
        }

        if (const auto length_us = lookup_integer(metadata, "mpris:length")) {
            state.info.length_us = std::clamp<gint64>(
                *length_us,
                0,
                kMaxMediaDurationUs
            );
        }

        state.info.track_id = lookup_string_like(
            metadata,
            "mpris:trackid",
            kMaxTrackIdBytes
        );

        state.info.artist = first_artist(metadata);
        g_variant_unref(metadata);
    }

    g_variant_unref(properties);
    g_variant_unref(reply);
    return state;
}

std::optional<PlayerState> select_player(GDBusConnection* connection) {
    const auto deadline = std::chrono::steady_clock::now() + kDiscoveryBudget;
    std::optional<PlayerState> fallback;
    for (const auto& name : list_players(connection, deadline)) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        auto player = read_player(connection, name, deadline);
        if (!player) continue;
        if (player->info.playback_status == 1) return player;
        if (!fallback || (fallback->info.playback_status == 0 && player->info.playback_status == 2)) {
            fallback = std::move(player);
        }
    }
    return fallback;
}

bool dictionary_contains_key(GVariant* dictionary, const char* key) {
    if (dictionary == nullptr || key == nullptr) return false;
    GVariant* value = g_variant_lookup_value(dictionary, key, nullptr);
    if (value == nullptr) return false;
    g_variant_unref(value);
    return true;
}

bool string_array_contains(GVariant* values, const char* expected) {
    if (values == nullptr || expected == nullptr) return false;
    GVariantIter iterator;
    const gchar* value = nullptr;
    g_variant_iter_init(&iterator, values);
    while (g_variant_iter_next(&iterator, "&s", &value)) {
        if (value != nullptr && std::string_view(value) == expected) return true;
    }
    return false;
}

bool relevant_player_properties_changed(GVariant* parameters) {
    if (parameters == nullptr) return true;

    const gchar* interface_name = nullptr;
    GVariant* changed = nullptr;
    GVariant* invalidated = nullptr;
    g_variant_get(
        parameters,
        "(&s@a{sv}@as)",
        &interface_name,
        &changed,
        &invalidated
    );

    constexpr const char* relevant_keys[]{
        "Metadata",
        "PlaybackStatus",
        "CanSeek",
        "Position",
    };
    bool relevant = interface_name != nullptr &&
        std::string_view(interface_name) == kPlayerInterface;
    if (relevant) {
        relevant = false;
        for (const char* key : relevant_keys) {
            if (dictionary_contains_key(changed, key) ||
                string_array_contains(invalidated, key)) {
                relevant = true;
                break;
            }
        }
    }

    if (changed != nullptr) g_variant_unref(changed);
    if (invalidated != nullptr) g_variant_unref(invalidated);
    return relevant;
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

void MediaService::reset_signal_monitor() {
    auto* connection = signal_connection_;
    if (connection == nullptr) {
        signal_closed_handler_id_ = 0;
        properties_subscription_id_ = 0;
        names_subscription_id_ = 0;
        return;
    }

    if (signal_closed_handler_id_ != 0) {
        g_signal_handler_disconnect(connection, signal_closed_handler_id_);
        signal_closed_handler_id_ = 0;
    }
    if (properties_subscription_id_ != 0) {
        g_dbus_connection_signal_unsubscribe(connection, properties_subscription_id_);
    }
    if (names_subscription_id_ != 0) {
        g_dbus_connection_signal_unsubscribe(connection, names_subscription_id_);
    }
    signal_connection_ = nullptr;
    properties_subscription_id_ = 0;
    names_subscription_id_ = 0;
    g_object_unref(connection);
}

void MediaService::schedule_signal_reconnect() {
    if (signal_reconnect_id_ != 0 ||
        signal_reconnect_attempts_ >= kMaxSignalReconnectAttempts) {
        return;
    }
    const guint delay_ms = std::min(
        kMaxSignalReconnectDelayMs,
        kSignalReconnectDelayMs << std::min(signal_reconnect_attempts_, 3U)
    );
    GSource* source = g_timeout_source_new(delay_ms);
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        +[](gpointer raw) -> gboolean {
            auto* self = static_cast<MediaService*>(raw);
            self->signal_reconnect_id_ = 0;
            if (self->ensure_signal_monitor()) {
                self->signal_reconnect_attempts_ = 0;
                return G_SOURCE_REMOVE;
            }
            ++self->signal_reconnect_attempts_;
            self->schedule_signal_reconnect();
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
    signal_reconnect_id_ = g_source_attach(source, signal_context_);
    g_source_unref(source);
}

MediaService::~MediaService() {
    if (signal_reconnect_id_ != 0) {
        g_source_remove(signal_reconnect_id_);
        signal_reconnect_id_ = 0;
    }
    reset_signal_monitor();
    if (signal_context_ != nullptr) {
        g_main_context_unref(signal_context_);
        signal_context_ = nullptr;
    }
}

bool MediaService::ensure_signal_monitor() {
    if (signal_connection_ != nullptr &&
        !g_dbus_connection_is_closed(signal_connection_) &&
        properties_subscription_id_ != 0 &&
        names_subscription_id_ != 0) {
        return true;
    }
    if (signal_connection_ != nullptr && g_dbus_connection_is_closed(signal_connection_)) {
        reset_signal_monitor();
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
    if (signal_connection_ == nullptr || g_dbus_connection_is_closed(signal_connection_)) {
        if (signal_connection_ != nullptr) reset_signal_monitor();
        g_clear_error(&error);
        schedule_signal_reconnect();
        return false;
    }
    g_dbus_connection_set_exit_on_close(signal_connection_, FALSE);
    signal_closed_handler_id_ = g_signal_connect(
        signal_connection_,
        "closed",
        G_CALLBACK(+[](GDBusConnection* connection, gboolean, GError*, gpointer data) {
            auto* self = static_cast<MediaService*>(data);
            if (self->signal_connection_ != connection) return;
            self->reset_signal_monitor();
            self->clear_cached_player();
            self->notify_changed();
            self->schedule_signal_reconnect();
        }),
        this
    );

    properties_subscription_id_ = g_dbus_connection_signal_subscribe(
        signal_connection_,
        nullptr,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        kObjectPath,
        kPlayerInterface,
        G_DBUS_SIGNAL_FLAGS_NONE,
        +[](
            GDBusConnection*,
            const gchar*,
            const gchar*,
            const gchar*,
            const gchar*,
            GVariant* parameters,
            gpointer data
        ) {
            if (!relevant_player_properties_changed(parameters)) return;
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
    if (properties_subscription_id_ != 0 && names_subscription_id_ != 0) {
        signal_reconnect_attempts_ = 0;
        return true;
    }

    if (properties_subscription_id_ != 0) {
        g_dbus_connection_signal_unsubscribe(signal_connection_, properties_subscription_id_);
    }
    if (names_subscription_id_ != 0) {
        g_dbus_connection_signal_unsubscribe(signal_connection_, names_subscription_id_);
    }
    if (signal_closed_handler_id_ != 0) {
        g_signal_handler_disconnect(signal_connection_, signal_closed_handler_id_);
        signal_closed_handler_id_ = 0;
    }
    g_object_unref(signal_connection_);
    signal_connection_ = nullptr;
    properties_subscription_id_ = 0;
    names_subscription_id_ = 0;
    schedule_signal_reconnect();
    return false;
}

MediaService::Subscription MediaService::subscribe(ChangedCallback callback) {
    if (!callback) return {};
    if (signal_context_ == nullptr) signal_context_ = g_main_context_ref_thread_default();
    static_cast<void>(ensure_signal_monitor());
    std::lock_guard lock(subscribers_->mutex);
    const std::size_t id = subscribers_->next_id++;
    subscribers_->callbacks.emplace(id, std::move(callback));
    return Subscription{subscribers_, id};
}

bool MediaService::signal_monitor_active() const {
    return signal_connection_ != nullptr &&
        !g_dbus_connection_is_closed(signal_connection_) &&
        properties_subscription_id_ != 0 &&
        names_subscription_id_ != 0;
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

bool MediaService::seek_to(
    std::string player_bus_name,
    std::string track_id,
    std::int64_t current_position_us,
    std::int64_t target_position_us
) {
    std::optional<std::string> player;
    if (!player_bus_name.empty()) player = std::move(player_bus_name);
    else player = current_player_name();
    if (!player) return false;

    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
        g_clear_error(&error);
        return false;
    }

    const gint64 safe_current_us = std::max<gint64>(0, current_position_us);
    const gint64 safe_target_us = std::max<gint64>(0, target_position_us);
    bool success = false;

    // SetPosition is the precise MPRIS operation, but a surprising number of
    // browser/player bridges either omit a usable track id or reject it. Try it
    // first when possible, then fall back to the relative Seek method.
    if (!track_id.empty() && track_id.front() == '/') {
        GVariant* reply = g_dbus_connection_call_sync(
            connection,
            player->c_str(),
            kObjectPath,
            kPlayerInterface,
            "SetPosition",
            g_variant_new("(ox)", track_id.c_str(), safe_target_us),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            kDbusTimeoutMs,
            nullptr,
            &error
        );
        success = reply != nullptr;
        if (reply != nullptr) g_variant_unref(reply);
        g_clear_error(&error);
    }

    if (!success) {
        const gint64 offset_us = safe_target_us - safe_current_us;
        if (offset_us == 0) {
            success = true;
        } else {
            GVariant* reply = g_dbus_connection_call_sync(
                connection,
                player->c_str(),
                kObjectPath,
                kPlayerInterface,
                "Seek",
                g_variant_new("(x)", offset_us),
                nullptr,
                G_DBUS_CALL_FLAGS_NONE,
                kDbusTimeoutMs,
                nullptr,
                &error
            );
            success = reply != nullptr;
            if (reply != nullptr) g_variant_unref(reply);
        }
    }

    if (!success && error != nullptr) {
        g_warning("Realmheart media seek failed: %s", error->message);
    }
    g_clear_error(&error);
    g_object_unref(connection);

    if (success) notify_changed();
    else clear_cached_player();
    return success;
}

} // namespace realmheart::services
