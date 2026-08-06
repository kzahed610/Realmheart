#pragma once

#include "ui/workspace/WorkspaceOverviewModel.hpp"

#include <array>
#include <functional>
#include <gtk/gtk.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace realmheart::ui::workspace {


class WorkspaceOverviewOverlay {
public:
    explicit WorkspaceOverviewOverlay(
        GtkApplication* app,
        std::function<void(int)> activate_workspace = {},
        std::function<void(int, std::string)> activate_window = {},
        std::function<void(int, std::string)> move_window = {}
    );
    ~WorkspaceOverviewOverlay();

    WorkspaceOverviewOverlay(const WorkspaceOverviewOverlay&) = delete;
    WorkspaceOverviewOverlay& operator=(const WorkspaceOverviewOverlay&) = delete;

    void show();
    void hide();
    void toggle();
    void set_workspace_snapshot(const services::WorkspaceSnapshot& snapshot);

    [[nodiscard]] bool visible() const;

private:
    struct CardAssets {
        GdkTexture* compact = nullptr;
        GdkTexture* expanded = nullptr;
    };

    struct OutgoingCard {
        CardAssets assets{};
        std::size_t slot = 0;
        bool active = false;
    };

    struct DragBounds {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    struct DragCard {
        CardAssets assets{};
        DragBounds compact_bounds{};
        DragBounds expanded_bounds{};
        std::string address;
        std::size_t realm_index = 0;
        std::size_t card_index = 0;
        double start_x = 0.0;
        double start_y = 0.0;
        double current_x = 0.0;
        double current_y = 0.0;
        double activity = 0.0;
        bool gesture_active = false;
        bool armed = false;
        bool active = false;
    };

    struct RealmAssets {
        GdkTexture* background = nullptr;
        GdkTexture* character = nullptr;
        PangoLayout* roman_layout = nullptr;
        PangoLayout* element_layout = nullptr;
        PangoLayout* place_layout = nullptr;
        GdkTexture* compact_overlay = nullptr;
        GdkTexture* expanded_overlay = nullptr;
        std::array<CardAssets, kWorkspaceOverviewCardLimit> cards{};
    };

    static void snapshot_callback(
        GtkWidget* widget,
        GtkSnapshot* snapshot,
        gpointer data
    );
    static gboolean animation_tick_callback(
        GtkWidget* widget,
        GdkFrameClock* frame_clock,
        gpointer data
    );

    void snapshot(GtkWidget* widget, GtkSnapshot* snapshot);
    void handle_primary_click(double x, double y);
    void handle_hover(double x, double y);
    bool handle_key_pressed(guint keyval);
    void move_realm_selection(int delta);
    void move_card_selection(int delta);
    void activate_selected();
    void normalize_selection() noexcept;
    void handle_drag_begin(double x, double y);
    void handle_drag_update(double offset_x, double offset_y);
    void handle_drag_end(double offset_x, double offset_y);
    void reset_drag() noexcept;
    void select_realm(int index);
    void stop_animation(bool snap_to_target) noexcept;
    void prepare_card_transition(const WorkspaceOverviewState& next);
    void finish_card_transition() noexcept;
    void ensure_animation_tick();
    void initialize_separator_nodes();
    void release_separator_nodes() noexcept;
    void synchronize_active_workspace();
    cairo_surface_t* application_icon_surface(
        std::string_view requested_icon_name,
        std::string_view app_name
    );
    void clear_icon_cache() noexcept;
    bool rebuild_dirty_overlays();
    bool ensure_assets();
    void release_assets() noexcept;

    GtkWindow* window_ = nullptr;
    GtkWidget* canvas_ = nullptr;
    std::array<RealmAssets, kWorkspaceOverviewRealmCount> assets_{};
    WorkspaceOverviewState workspace_state_{};
    std::array<bool, kWorkspaceOverviewRealmCount> overlay_dirty_{{
        true, true, true, true,
    }};
    std::array<GskRenderNode*, 3> separator_nodes_{};
    std::array<double, 4> displayed_heights_{};
    std::array<double, 4> animation_start_heights_{};
    std::array<double, 4> animation_target_heights_{};
    std::array<std::array<int, kWorkspaceOverviewCardLimit>,
        kWorkspaceOverviewRealmCount> card_from_slots_{};
    std::array<std::array<bool, kWorkspaceOverviewCardLimit>,
        kWorkspaceOverviewRealmCount> card_entering_{};
    std::array<std::array<OutgoingCard, kWorkspaceOverviewCardLimit>,
        kWorkspaceOverviewRealmCount> outgoing_cards_{};
    std::function<void(int)> activate_workspace_;
    std::function<void(int, std::string)> activate_window_;
    std::function<void(int, std::string)> move_window_;
    std::unordered_map<std::string, cairo_surface_t*> icon_surfaces_;
    DragCard drag_card_{};
    int drag_target_index_ = -1;
    std::string asset_error_;
    int active_index_ = 1;
    int selected_card_index_ = -1;
    guint animation_tick_id_ = 0;
    gint64 animation_start_time_us_ = 0;
    gint64 card_animation_start_time_us_ = 0;
    double card_animation_progress_ = 1.0;
    bool realm_animation_active_ = false;
    bool card_animation_active_ = false;
    bool assets_attempted_ = false;
};

} // namespace realmheart::ui::workspace
