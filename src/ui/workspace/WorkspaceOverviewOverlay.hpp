#pragma once

#include "effects/core/TransitionTimeline.hpp"
#include "ui/workspace/WorkspaceOverviewModel.hpp"
#include "ui/workspace/animation/WorkspaceMorphDiagnostics.hpp"
#include "ui/workspace/animation/WorkspaceMorphModel.hpp"
#include "ui/workspace/animation/WorkspaceOverviewMorphRenderer.hpp"

#include <array>
#include <functional>
#include <gtk/gtk.h>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realmheart::ui::workspace {


class WorkspaceOverviewOverlay {
public:
    explicit WorkspaceOverviewOverlay(
        GtkApplication* app,
        std::function<void(int)> activate_workspace = {},
        std::function<void(int, std::string)> activate_window = {},
        std::function<void(int, std::string)> move_window = {},
        std::function<void(bool)> set_taskbar_morph_active = {},
        std::function<void(double)> set_taskbar_morph_progress = {}
    );
    ~WorkspaceOverviewOverlay();

    WorkspaceOverviewOverlay(const WorkspaceOverviewOverlay&) = delete;
    WorkspaceOverviewOverlay& operator=(const WorkspaceOverviewOverlay&) = delete;

    void show();
    void hide();
    void toggle();
    void set_workspace_snapshot(const services::WorkspaceSnapshot& snapshot);
    void set_morph_sources(
        std::vector<animation::WorkspaceMorphSource> sources
    );

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
        int source_workspace_id = 0;
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
        int roman_workspace_id = 0;
        PangoLayout* element_layout = nullptr;
        PangoLayout* place_layout = nullptr;
        GdkTexture* compact_overlay = nullptr;
        GdkTexture* expanded_overlay = nullptr;
        std::array<CardAssets, kWorkspaceOverviewCardLimit> cards{};
        std::vector<GdkTexture*> spread_cards{};
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
    void expand_overflow(std::size_t realm_index);
    void collapse_overflow(bool snap = false) noexcept;
    [[nodiscard]] bool overflow_expanded_for(
        std::size_t realm_index
    ) const noexcept;
    void select_realm(int index, int preferred_card_index = -2);
    void transition_selection(int realm_index, int card_index);
    [[nodiscard]] std::optional<graphene_rect_t>
        selected_card_outline_bounds() const;
    void finish_selection_transition() noexcept;
    void stop_content_animations(bool snap_to_target) noexcept;
    void stop_animation(bool snap_to_target) noexcept;
    void prepare_card_transition(const WorkspaceOverviewState& next);
    void finish_card_transition() noexcept;
    void ensure_animation_tick();
    void set_morph_input_enabled(bool enabled) noexcept;
    void capture_morph_geometry() noexcept;
    void schedule_morph_shader_capture() noexcept;
    void try_begin_morph_shader() noexcept;
    void update_morph_shader() noexcept;
    void finish_morph_endpoint() noexcept;
    [[nodiscard]] bool morph_interactive() const noexcept;
    [[nodiscard]] animation::WorkspaceMorphLayout morph_layout(
        double scale_x,
        double scale_y
    ) const noexcept;
    void initialize_separator_nodes();
    void release_separator_nodes() noexcept;
    void synchronize_active_workspace();
    void begin_viewport_transition(int direction);
    [[nodiscard]] std::size_t style_index_for_realm(
        std::size_t realm_index
    ) const noexcept;
    [[nodiscard]] double viewport_visual_offset() const noexcept;
    cairo_surface_t* application_icon_surface(
        std::string_view requested_icon_name,
        std::string_view app_name
    );
    void clear_icon_cache() noexcept;
    bool rebuild_dirty_overlays();
    bool ensure_assets();
    void release_assets() noexcept;

    GtkWindow* window_ = nullptr;
    GtkWidget* overlay_stack_ = nullptr;
    GtkWidget* canvas_ = nullptr;
    animation::WorkspaceOverviewMorphRenderer morph_renderer_{};
    animation::WorkspaceMorphDiagnostics morph_diagnostics_{};
    std::array<RealmAssets, kWorkspaceOverviewRealmCount> assets_{};
    WorkspaceOverviewState workspace_state_{};
    std::array<bool, kWorkspaceOverviewRealmCount> overlay_dirty_{{
        true, true, true, true,
    }};
    std::array<std::array<GskRenderNode*, kWorkspaceOverviewRealmCount>, 3>
        separator_nodes_{};
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
    std::function<void(bool)> set_taskbar_morph_active_;
    std::function<void(double)> set_taskbar_morph_progress_;
    std::vector<animation::WorkspaceMorphSource> morph_sources_;
    std::array<int, animation::kWorkspaceMorphBandCount> morph_workspace_ids_{};
    std::array<double, animation::kWorkspaceMorphBandCount>
        morph_destination_heights_{};
    effects::TransitionTimeline morph_timeline_{{0.50, 0.34}};
    std::unordered_map<std::string, cairo_surface_t*> icon_surfaces_;
    DragCard drag_card_{};
    int drag_target_index_ = -1;
    int overflow_workspace_id_ = 0;
    std::string asset_error_;
    int viewport_start_workspace_id_ = 1;
    int viewport_transition_direction_ = 0;
    int active_index_ = 1;
    int selected_card_index_ = -1;
    guint animation_tick_id_ = 0;
    gint64 animation_start_time_us_ = 0;
    gint64 card_animation_start_time_us_ = 0;
    gint64 overflow_animation_start_time_us_ = 0;
    gint64 selection_animation_start_time_us_ = 0;
    gint64 viewport_animation_start_time_us_ = 0;
    gint64 morph_last_frame_time_us_ = 0;
    graphene_rect_t selection_outline_start_bounds_{};
    double card_animation_progress_ = 1.0;
    double overflow_animation_progress_ = 0.0;
    double overflow_animation_start_progress_ = 0.0;
    double overflow_animation_target_ = 0.0;
    double selection_animation_progress_ = 1.0;
    double viewport_animation_progress_ = 1.0;
    double selection_outline_start_opacity_ = 0.0;
    bool realm_animation_active_ = false;
    bool card_animation_active_ = false;
    bool overflow_animation_active_ = false;
    bool selection_animation_active_ = false;
    bool viewport_animation_active_ = false;
    bool selection_outline_has_start_bounds_ = false;
    bool assets_attempted_ = false;
    bool taskbar_morph_active_ = false;
    bool morph_geometry_frozen_ = false;
    bool morph_shader_capture_pending_ = false;
    bool morph_shader_failed_for_transition_ = false;
    bool force_native_capture_ = false;
};

} // namespace realmheart::ui::workspace
