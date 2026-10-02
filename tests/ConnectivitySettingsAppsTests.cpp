#include "ui/sidebar/ConnectivitySettingsApps.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace realmheart::ui::sidebar {
namespace {

TEST(ConnectivitySettingsAppsTests, WifiCandidatesPreferNetworkManagerThenKdeAndGnome) {
    const auto candidates = connectivity_settings_candidates(ConnectivitySettingsService::Wifi);

    ASSERT_EQ(candidates.size(), 3U);
    EXPECT_EQ(candidates[0].desktop_id, "nm-connection-editor.desktop");
    EXPECT_EQ(candidates[1].desktop_id, "kcm_networkmanagement.desktop");
    EXPECT_EQ(candidates[2].executable, "gnome-control-center");
    EXPECT_EQ(candidates[2].argument, "wifi");
}

TEST(ConnectivitySettingsAppsTests, BluetoothCandidatesIncludeKdeBluemanAndGnome) {
    const auto candidates = connectivity_settings_candidates(ConnectivitySettingsService::Bluetooth);

    ASSERT_EQ(candidates.size(), 3U);
    EXPECT_EQ(candidates[0].desktop_id, "kcm_bluetooth.desktop");
    EXPECT_EQ(candidates[1].desktop_id, "blueman-manager.desktop");
    EXPECT_EQ(candidates[2].executable, "gnome-control-center");
    EXPECT_EQ(candidates[2].argument, "bluetooth");
}

TEST(ConnectivitySettingsAppsTests, UsesFirstInstalledCandidateThatLaunches) {
    const auto candidates = connectivity_settings_candidates(ConnectivitySettingsService::Wifi);
    std::vector<std::string> attempts;
    const auto result = launch_available_connectivity_settings(
        ConnectivitySettingsService::Wifi,
        [](const SettingsAppCandidate& candidate) {
            return candidate.desktop_id == "kcm_networkmanagement.desktop" ||
                candidate.executable == "gnome-control-center";
        },
        [&attempts](const SettingsAppCandidate& candidate) {
            attempts.emplace_back(candidate.desktop_id.empty()
                ? candidate.executable
                : candidate.desktop_id);
            return candidate.executable == "gnome-control-center";
        }
    );

    EXPECT_EQ(result, SettingsAppLaunchStatus::Opened);
    ASSERT_EQ(attempts.size(), 2U);
    EXPECT_EQ(attempts[0], candidates[1].desktop_id);
    EXPECT_EQ(attempts[1], "gnome-control-center");
}

TEST(ConnectivitySettingsAppsTests, ReportsUnavailableWhenNoCandidateIsInstalled) {
    int launch_attempts = 0;
    const auto result = launch_available_connectivity_settings(
        ConnectivitySettingsService::Bluetooth,
        [](const SettingsAppCandidate&) { return false; },
        [&launch_attempts](const SettingsAppCandidate&) {
            ++launch_attempts;
            return true;
        }
    );

    EXPECT_EQ(result, SettingsAppLaunchStatus::Unavailable);
    EXPECT_EQ(launch_attempts, 0);
}

TEST(ConnectivitySettingsAppsTests, KeepsInstallSuggestionsServiceSpecific) {
    EXPECT_NE(
        connectivity_settings_install_suggestion(ConnectivitySettingsService::Wifi)
            .find("nm-connection-editor"),
        std::string_view::npos
    );
    EXPECT_NE(
        connectivity_settings_install_suggestion(ConnectivitySettingsService::Bluetooth)
            .find("blueman"),
        std::string_view::npos
    );
}

} // namespace
} // namespace realmheart::ui::sidebar
