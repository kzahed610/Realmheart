#include "services/BatteryService.hpp"
#include "services/MediaService.hpp"

#include <gio/gio.h>

#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_battery_fixture_contracts() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-bar-services-battery";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "73\n";
    std::ofstream(root / "BAT0/status") << "Charging\n";
    std::ofstream(root / "BAT0/power_now") << "12340000\n";

    realmheart::services::BatteryService battery(root);
    const auto valid = battery.read();
    require(valid.has_value(), "fixture battery must be detected");
    require(valid->percentage == 73, "fixture battery capacity must be parsed");
    require(valid->charging, "fixture battery charging transition must be parsed");
    require(valid->rate_watts.has_value(), "fixture battery power rate must be present");

    std::ofstream(root / "BAT0/capacity", std::ios::trunc) << "not-a-number\n";
    require(!battery.read().has_value(), "malformed battery capacity must be unavailable");

    std::filesystem::remove_all(root, error);
    require(!battery.read().has_value(), "removed battery fixture must be unavailable");
}

struct FakeMediaState {
    std::mutex mutex;
    bool malformed = false;
    bool playing = true;
    std::string title = "Fixture track";
    std::string artist = "Fixture artist";
};

constexpr const char* kMediaName = "org.mpris.MediaPlayer2.realmheart_fixture";
constexpr const char* kMediaPath = "/org/mpris/MediaPlayer2";
constexpr const char* kMediaInterface = "org.mpris.MediaPlayer2.Player";
constexpr guint kDbusRequestNamePrimaryOwner = 1U;

void fake_media_method_call(
    GDBusConnection*,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar* method,
    GVariant* parameters,
    GDBusMethodInvocation* invocation,
    gpointer user_data
) {
    auto* state = static_cast<FakeMediaState*>(user_data);
    if (g_strcmp0(method, "GetAll") != 0) {
        g_dbus_method_invocation_return_error(
            invocation,
            G_IO_ERROR,
            G_IO_ERROR_NOT_SUPPORTED,
            "unsupported fixture method"
        );
        return;
    }

    const gchar* requested_interface = nullptr;
    g_variant_get(parameters, "(&s)", &requested_interface);
    if (g_strcmp0(requested_interface, kMediaInterface) != 0) {
        g_dbus_method_invocation_return_error(
            invocation,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "unexpected fixture interface"
        );
        return;
    }

    std::lock_guard lock(state->mutex);
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    if (state->malformed) {
        g_variant_builder_add(&properties, "{sv}", "PlaybackStatus", g_variant_new_int32(7));
        g_variant_builder_add(&properties, "{sv}", "CanSeek", g_variant_new_string("yes"));
        g_variant_builder_add(&properties, "{sv}", "Position", g_variant_new_string("bad"));
        g_variant_builder_add(&properties, "{sv}", "Metadata", g_variant_new_string("bad"));
    } else {
        g_variant_builder_add(
            &properties,
            "{sv}",
            "PlaybackStatus",
            g_variant_new_string(state->playing ? "Playing" : "Paused")
        );
        g_variant_builder_add(&properties, "{sv}", "CanSeek", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&properties, "{sv}", "Position", g_variant_new_int64(2'000'000));

        GVariantBuilder metadata;
        g_variant_builder_init(&metadata, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(
            &metadata,
            "{sv}",
            "xesam:title",
            g_variant_new_string(state->title.c_str())
        );
        GVariantBuilder artists;
        g_variant_builder_init(&artists, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&artists, "s", state->artist.c_str());
        g_variant_builder_add(
            &metadata,
            "{sv}",
            "xesam:artist",
            g_variant_builder_end(&artists)
        );
        g_variant_builder_add(
            &metadata,
            "{sv}",
            "mpris:length",
            g_variant_new_int64(180'000'000)
        );
        g_variant_builder_add(
            &metadata,
            "{sv}",
            "mpris:trackid",
            g_variant_new_object_path("/org/mpris/MediaPlayer2/track/fixture")
        );
        g_variant_builder_add(
            &metadata,
            "{sv}",
            "mpris:artUrl",
            g_variant_new_string("file:///tmp/realmheart-fixture.png")
        );
        g_variant_builder_add(
            &properties,
            "{sv}",
            "Metadata",
            g_variant_builder_end(&metadata)
        );
    }

    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(@a{sv})", g_variant_builder_end(&properties))
    );
}

class FakeMediaBus {
public:
    ~FakeMediaBus() { stop(); }

    bool start(const char* address) {
        if (address == nullptr || *address == '\0') return false;
        thread_ = std::thread([this, address = std::string(address)] { run(address); });
        std::unique_lock lock(mutex_);
        ready_cv_.wait(lock, [this] { return ready_; });
        return !failed_;
    }

    void set_malformed(bool malformed) {
        std::lock_guard lock(state_.mutex);
        state_.malformed = malformed;
    }

    void set_playing(bool playing) {
        std::lock_guard lock(state_.mutex);
        state_.playing = playing;
    }

private:
    static const GDBusInterfaceVTable& vtable() {
        static const GDBusInterfaceVTable table = [] {
            GDBusInterfaceVTable value{};
            value.method_call = fake_media_method_call;
            return value;
        }();
        return table;
    }

    static gboolean quit_loop(gpointer data) {
        g_main_loop_quit(static_cast<GMainLoop*>(data));
        return G_SOURCE_REMOVE;
    }

    void run(const std::string& address) {
        GMainContext* context = g_main_context_new();
        g_main_context_push_thread_default(context);
        GError* error = nullptr;
        connection_ = g_dbus_connection_new_for_address_sync(
            address.c_str(),
            static_cast<GDBusConnectionFlags>(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
            ),
            nullptr,
            nullptr,
            &error
        );
        if (connection_ == nullptr) {
            g_clear_error(&error);
            mark_ready(true);
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            return;
        }
        g_dbus_connection_set_exit_on_close(connection_, FALSE);

        GVariant* reply = g_dbus_connection_call_sync(
            connection_,
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "RequestName",
            g_variant_new("(su)", kMediaName, 0U),
            G_VARIANT_TYPE("(u)"),
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            nullptr,
            &error
        );
        if (reply == nullptr) {
            g_clear_error(&error);
            mark_ready(true);
            g_object_unref(connection_);
            connection_ = nullptr;
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            return;
        }
        guint request_result = 0U;
        g_variant_get(reply, "(u)", &request_result);
        g_variant_unref(reply);
        if (request_result != kDbusRequestNamePrimaryOwner) {
            mark_ready(true);
            g_object_unref(connection_);
            connection_ = nullptr;
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            return;
        }

        const char* introspection =
            "<node><interface name='org.freedesktop.DBus.Properties'>"
            "<method name='GetAll'><arg type='s' direction='in'/>"
            "<arg type='a{sv}' direction='out'/></method>"
            "</interface></node>";
        node_info_ = g_dbus_node_info_new_for_xml(introspection, &error);
        if (node_info_ == nullptr) {
            g_clear_error(&error);
            mark_ready(true);
            g_object_unref(connection_);
            connection_ = nullptr;
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            return;
        }
        registration_id_ = g_dbus_connection_register_object(
            connection_,
            kMediaPath,
            node_info_->interfaces[0],
            &vtable(),
            &state_,
            nullptr,
            &error
        );
        if (registration_id_ == 0) {
            g_clear_error(&error);
            mark_ready(true);
            g_dbus_node_info_unref(node_info_);
            node_info_ = nullptr;
            g_object_unref(connection_);
            connection_ = nullptr;
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
            return;
        }

        loop_ = g_main_loop_new(context, FALSE);
        {
            std::lock_guard lock(mutex_);
            context_ = context;
            ready_ = true;
        }
        ready_cv_.notify_one();
        g_main_loop_run(loop_);

        g_dbus_connection_unregister_object(connection_, registration_id_);
        registration_id_ = 0;
        g_dbus_node_info_unref(node_info_);
        node_info_ = nullptr;
        g_object_unref(connection_);
        connection_ = nullptr;
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
    }

    void mark_ready(bool failed) {
        std::lock_guard lock(mutex_);
        ready_ = true;
        failed_ = failed;
        ready_cv_.notify_one();
    }

    void stop() {
        GMainContext* context = nullptr;
        GMainLoop* loop = nullptr;
        {
            std::lock_guard lock(mutex_);
            context = context_;
            loop = loop_;
        }
        if (context != nullptr && loop != nullptr) {
            g_main_context_invoke(context, quit_loop, loop);
        }
        if (thread_.joinable()) thread_.join();
    }

    FakeMediaState state_;
    std::mutex mutex_;
    std::condition_variable ready_cv_;
    bool ready_ = false;
    bool failed_ = false;
    std::thread thread_;
    GMainContext* context_ = nullptr;
    GMainLoop* loop_ = nullptr;
    GDBusConnection* connection_ = nullptr;
    GDBusNodeInfo* node_info_ = nullptr;
    guint registration_id_ = 0;
};

void test_media_fixture_contracts() {
    GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);

    {
        FakeMediaBus fixture;
        require(
            fixture.start(g_test_dbus_get_bus_address(bus)),
            "fixture media bus must start"
        );

        realmheart::services::MediaService media;
        const auto playing = media.get_current_media();
        require(playing.has_value(), "fixture media must be detected");
        require(playing->title == "Fixture track", "fixture media title must be parsed");
        require(playing->artist == "Fixture artist", "fixture media artist must be parsed");
        require(playing->playback_status == 1, "playing transition must be parsed");
        require(playing->length_us == 180'000'000, "fixture media duration must be parsed");

        fixture.set_playing(false);
        const auto paused = media.get_current_media();
        require(paused.has_value(), "paused fixture media must remain available");
        require(paused->playback_status == 2, "paused transition must be parsed");

        fixture.set_malformed(true);
        const auto malformed = media.get_current_media();
        require(malformed.has_value(), "malformed optional properties must not discard player");
        require(malformed->playback_status == 0, "malformed playback status must use stopped fallback");
        require(malformed->position_us == 0, "malformed position must use zero fallback");
    }

    realmheart::services::MediaService unavailable_media;
    require(!unavailable_media.get_current_media().has_value(),
            "missing media player must be unavailable");

    g_test_dbus_down(bus);
    g_object_unref(bus);
}

} // namespace

int main() {
    test_battery_fixture_contracts();
    test_media_fixture_contracts();
    std::cout << "Bar service fixture tests PASSED\n";
    return 0;
}
