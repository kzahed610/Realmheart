#pragma once

#include "core/DisplayTier.hpp"
#include "animation/character/CharacterHairMode.hpp"
#include "animation/character/CharacterQualityPreset.hpp"
#include "ui/sidebar/SidebarGeometry.hpp"
#include "effects/core/EffectFrame.hpp"
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
#include <string_view>
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
    int monitor_width = 0;
    int monitor_height = 0;
    core::DisplayTier display_tier = core::DisplayTier::P1080;
    SidebarFrameLayout frame_layout = kDefaultSidebarFrameLayout;
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
    void apply_geometry();
    void toggle_character();
    bool set_character_hair_mode(std::string_view mode_name);
    // Future settings-panel hook. Deliberately not wired to the sidebar's
    // existing system power-profile buttons.
    bool set_character_quality_preset(
        realmheart::animation::character::CharacterQualityPreset preset
    );
    void animate_character_in();
    void set_surface_effect(effects::EffectId effect, double progress);
    [[nodiscard]] bool animate_character_out(std::function<void()> completion);
    [[nodiscard]] bool character_enabled() const noexcept { return character_enabled_; }
    GtkWidget* get_window() const { return window_; }
    // One-time boot warmup: maps the panel once at opacity 0 for a single
    // frame tick so the first real open skips first-map pipeline costs.
    // No-op after the first call; never grabs keyboard (OnDemand mode).
    void prewarm();
    // Aborts an in-flight prewarm frame (restores opacity, drops the tick).
    // Called by the real toggle path so a warm-frame unmap can never fight
    // a genuine open.
    void cancel_prewarm();

private:
    void update_geometry_on_realize(GtkWidget* widget);
    static gboolean retry_geometry(gpointer raw);
    void schedule_geometry_retry();
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
    bool apply_character_hair_mode(
        realmheart::animation::character::CharacterHairMode mode
    );
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
    GtkWidget* effect_view_ = nullptr;
    GtkWidget* container_ = nullptr;
    std::unique_ptr<SidebarFrame> frame_;
    std::unique_ptr<realmheart::animation::character::CharacterCompositor> character_compositor_;
    bool character_enabled_ = true;
    realmheart::animation::character::CharacterHairMode character_hair_mode_ =
        realmheart::animation::character::CharacterHairMode::Mesh;
    bool sidebar_presented_ = false;
    bool prewarmed_ = false;
    bool prewarm_frame_active_ = false;
    core::DisplayTier display_tier_ = core::DisplayTier::P1080;
    SidebarFrameLayout frame_layout_ = kDefaultSidebarFrameLayout;
    int sidebar_height_ = 760;
    bool geometry_initialized_ = false;
    guint geometry_retry_id_ = 0;
    guint prewarm_tick_id_ = 0;
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
