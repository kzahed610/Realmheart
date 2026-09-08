#include "services/Notifications.hpp"
#include "services/RightSidebarServices.hpp"
#include "ui/sidebar/NightLightTileState.hpp"
#include "ui/sidebar/SidebarPreferences.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class EnvironmentGuard {
public:
    explicit EnvironmentGuard(const char* name)
        : name_(name) {
        if (const char* value = std::getenv(name_); value != nullptr) {
            previous_ = value;
        }
    }

    ~EnvironmentGuard() {
        if (previous_) setenv(name_, previous_->c_str(), 1);
        else unsetenv(name_);
    }

private:
    const char* name_;
    std::optional<std::string> previous_;
};

void test_private_atomic_preferences_and_symlink_rejection() {
    EnvironmentGuard xdg_config_home("XDG_CONFIG_HOME");
    EnvironmentGuard home("HOME");
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-task18-" + std::to_string(static_cast<unsigned long long>(::getpid())));
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    require(setenv("XDG_CONFIG_HOME", root.c_str(), 1) == 0,
            "test must set an isolated XDG_CONFIG_HOME");
    unsetenv("HOME");

    const auto path = realmheart::ui::sidebar::sidebar_preference_path(
        "test-preference"
    );
    require(path.has_value(), "preference path should resolve below private storage");
    require(realmheart::ui::sidebar::write_sidebar_preference(
                "test-preference", "enabled\n"
            ),
            "preference write should succeed");
    const auto stored = realmheart::ui::sidebar::read_sidebar_preference(
        "test-preference"
    );
    require(stored && *stored == "enabled\n", "preference read should round-trip");

    struct stat metadata{};
    require(::stat(path->c_str(), &metadata) == 0, "preference file should exist");
    require((metadata.st_mode & 0077U) == 0U,
            "preference file must not be group/world accessible");
    struct stat directory_metadata{};
    require(::stat(path->parent_path().c_str(), &directory_metadata) == 0,
            "preference directory should exist");
    require((directory_metadata.st_mode & 0077U) == 0U,
            "preference directory must be private");

    const auto external = root / "external-target";
    {
        std::ofstream output(external);
        output << "keep-me\n";
    }
    std::filesystem::remove(*path);
    require(::symlink(external.c_str(), path->c_str()) == 0,
            "test must create the hostile symlink fixture");
    require(!realmheart::ui::sidebar::write_sidebar_preference(
                "test-preference", "must-not-follow\n"
            ),
            "preference writer must reject a symlink destination");
    require(std::filesystem::is_symlink(*path),
            "hostile symlink must remain untouched");
    std::ifstream input(external);
    std::string external_value;
    std::getline(input, external_value);
    require(external_value == "keep-me",
            "symlink target must not be overwritten");

    std::filesystem::remove_all(root, cleanup_error);
}

void test_notification_status_uses_live_history_contract() {
    realmheart::services::NotificationHistory history;
    realmheart::services::RightSidebarServices services({}, &history);

    const auto disconnected = services.getNotificationsStatus();
    require(!disconnected.enabled, "inactive notification daemon must be unavailable");
    require(disconnected.status.find("daemon is inactive") != std::string::npos,
            "inactive notification status must state the live failure");

    history.upsert({
        .id = 7,
        .app_name = "test",
        .summary = "Unread",
        .body = "Body",
        .unread = true,
    });
    history.set_capture_active(true);
    const auto connected = services.getNotificationsStatus();
    require(connected.enabled, "active notification daemon must be available");
    require(connected.status.find("1 unread") != std::string::npos,
            "notification status must report live unread count");
    require(connected.status.find("1 stored") != std::string::npos,
            "notification status must report live history size");
}

void test_stopped_night_light_tile_remains_actionable() {
    const auto [stopped_status, stopped_active, stopped_available] =
        realmheart::ui::sidebar::night_light_tile_presentation(
            std::nullopt,
            true
        );
    require(stopped_status == "Start",
            "installed but stopped Night Light must expose a Start tile action");
    require(!stopped_active, "stopped Night Light tile must not appear active");
    require(stopped_available,
            "installed but stopped Night Light tile must remain clickable");

    const auto [unavailable_status, unavailable_active, unavailable_available] =
        realmheart::ui::sidebar::night_light_tile_presentation(
            std::nullopt,
            false
        );
    require(unavailable_status == "Unavailable",
            "missing Night Light backend must remain unavailable");
    require(!unavailable_active && !unavailable_available,
            "missing Night Light backend must remain inactive and disabled");

    const auto [live_status, live_active, live_available] =
        realmheart::ui::sidebar::night_light_tile_presentation(
            false,
            true
        );
    require(live_status == "Off" && !live_active && live_available,
            "verified live Night Light state must retain normal tile semantics");
}

} // namespace

int main() {
    test_private_atomic_preferences_and_symlink_rejection();
    test_notification_status_uses_live_history_contract();
    test_stopped_night_light_tile_remains_actionable();
    std::cout << "Right sidebar integration tests passed\n";
    return 0;
}
