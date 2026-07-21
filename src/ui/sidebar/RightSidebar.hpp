#pragma once

#include "services/KeepAwake.hpp"
#include "services/Notifications.hpp"
#include "ui/components/BaseWidget.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace realmheart::animation::character {
class CharacterCompositor;
}

namespace realmheart::ui::components {
class SliderWidget;
}

namespace realmheart::ui::sidebar {

struct SidebarPlacement {
    int height = 760;
    int top_margin = 76;
};

// Shared by the sidebar surface and its edge hotspot so both remain exactly
// aligned if the height fraction changes later.
[[nodiscard]] SidebarPlacement sidebar_placement_for(GtkWidget* widget);

class SidebarFrame;
class QuickControlTile;
class WifiManagerPopover;
class BluetoothManagerPopover;
class NightLightPanel;

class RightSidebar {
public:
    RightSidebar(
        GtkApplication* app,
        services::NotificationHistory& notification_history,
        std::function<void(double)> show_volume_osd = {},
        std::function<void(double)> show_brightness_osd = {}
    );
    ~RightSidebar();

    RightSidebar(const RightSidebar&) = delete;
    RightSidebar& operator=(const RightSidebar&) = delete;

    void refresh();
    void toggle_character();
    void animate_character_in();
    [[nodiscard]] bool animate_character_out(std::function<void()> completion);
    [[nodiscard]] bool character_enabled() const noexcept { return character_enabled_; }
    GtkWidget* get_window() const { return window_; }

private:
    struct AsyncUiState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> generation{0};
        std::atomic<bool> refresh_in_flight{false};
        std::atomic<bool> refresh_pending{false};
        std::mutex operation_mutex;
        RightSidebar* owner = nullptr; // GTK main thread only
    };

    struct ControlRefreshResult {
        std::shared_ptr<AsyncUiState> state;
        std::optional<std::string> wifi_status;
        bool wifi_active = false;
        std::optional<bool> bluetooth_powered;
        std::optional<bool> night_light_enabled;
        std::optional<std::string> active_profile;
        std::optional<double> brightness_percent;
        std::optional<double> volume_percent;
        std::uint64_t brightness_generation = 0;
        std::uint64_t volume_generation = 0;
    };

    void setup_layout();
    void initialize_character_compositor();
    void cancel_character_hide_timeout();
    static gboolean finish_character_hide(gpointer raw);
    void set_character_enabled(bool enabled);
    void populate_modules();
    void build_identity_header();
    void build_quick_controls();
    void build_power_profiles();
    void install_panel_click_away();
    void refresh_controls();
    static gboolean finish_control_refresh(gpointer raw);
    static void destroy_control_refresh_result(gpointer raw);
    void post_control_action(std::function<void()> action);
    void set_power_profile(const std::string& profile);
    void show_power_profile_feedback(GtkWidget* button, bool success);
    void clear_power_profile_feedback();

    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* content_overlay_ = nullptr;
    GtkWidget* container_ = nullptr;
    std::unique_ptr<SidebarFrame> frame_;
    std::unique_ptr<realmheart::animation::character::CharacterCompositor> character_compositor_;
    bool character_enabled_ = true;
    bool sidebar_presented_ = false;
    guint character_hide_timeout_ = 0;
    std::function<void()> character_hide_completion_;
    std::vector<std::unique_ptr<components::BaseWidget>> modules_;
    std::shared_ptr<services::KeepAwake> keep_awake_;
    services::NotificationHistory& notification_history_;
    std::function<void(double)> show_volume_osd_;
    std::function<void(double)> show_brightness_osd_;
    GtkWidget* online_label_ = nullptr;
    GtkWidget* uptime_label_ = nullptr;
    std::array<GtkWidget*, 3> power_profile_buttons_{};
    std::string pending_power_profile_;
    guint power_feedback_timeout_ = 0;
    std::unique_ptr<QuickControlTile> wifi_tile_;
    std::unique_ptr<QuickControlTile> bluetooth_tile_;
    std::unique_ptr<QuickControlTile> night_light_tile_;
    std::unique_ptr<QuickControlTile> keep_awake_tile_;
    components::SliderWidget* brightness_slider_ = nullptr;
    components::SliderWidget* volume_slider_ = nullptr;
    std::unique_ptr<WifiManagerPopover> wifi_panel_;
    std::unique_ptr<BluetoothManagerPopover> bluetooth_panel_;
    std::unique_ptr<NightLightPanel> night_light_panel_;
    std::shared_ptr<AsyncUiState> async_ui_state_ = std::make_shared<AsyncUiState>();
};

} // namespace realmheart::ui::sidebar
