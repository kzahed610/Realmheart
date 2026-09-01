#pragma once

#include "effects/core/TransitionTimeline.hpp"
#include "services/LauncherService.hpp"
#include "services/WallpaperService.hpp"
#include "ui/launcher/LauncherGeometry.hpp"

#include <gtk/gtk.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realmheart::effects::shell {
class ShellShaderRenderer;
}

namespace realmheart::ui {

class CommandReceiptOverlay;

class LauncherOverlay {
public:
    LauncherOverlay(
        GtkApplication* app,
        services::LauncherService& service,
        services::WallpaperService& wallpaper_service,
        CommandReceiptOverlay& command_receipts,
        int monitor_index = -1
    );
    ~LauncherOverlay();

    void toggle();
    void show();
    void show_with_query(std::string query);
    void hide();

private:
    enum class SearchMode {
        Normal,
        Clipboard,
        ClipboardClear,
        Emoji,
    };

    enum class SpatialDirection {
        Left,
        Right,
        Up,
        Down,
    };

    struct ConstellationPlacement {
        std::string application_id;
        double normalized_x = 0.5;
        double normalized_y = 0.5;
    };

    struct ResultRowMotion {
        GtkListBoxRow* row = nullptr;
        GtkWidget* content = nullptr;
        double lift = 0.0;
        double velocity = 0.0;

        ResultRowMotion() = default;
        ResultRowMotion(const ResultRowMotion&) = delete;
        ResultRowMotion& operator=(const ResultRowMotion&) = delete;
        ResultRowMotion(ResultRowMotion&&) = delete;
        ResultRowMotion& operator=(ResultRowMotion&&) = delete;
        ~ResultRowMotion();

        void bind(GtkListBoxRow* new_row, GtkWidget* new_content);
    };


    struct ClipboardAsyncState;
    struct EmojiAsyncState;

    struct ClipboardThumbnail {
        GdkTexture* texture = nullptr;
        int source_width = 0;
        int source_height = 0;
        std::string format;
    };

    struct ClipboardRowWidgets {
        GtkWidget* row = nullptr;
        GtkWidget* icon_slot = nullptr;
        GtkWidget* subtitle = nullptr;
        std::uint64_t view_generation = 0;
        bool thumbnail_visible = false;
    };

    struct ConstellationNode {
        services::LauncherResult result;
        GtkWidget* widget = nullptr;
        GtkWidget* menu = nullptr;
        double normalized_x = 0.5;
        double normalized_y = 0.5;
        double drag_grab_x = 0.0;
        double drag_grab_y = 0.0;
        double drag_pointer_start_x = 0.0;
        double drag_pointer_start_y = 0.0;
        double current_x = 0.0;
        double current_y = 0.0;
        double render_x = 0.0;
        double render_y = 0.0;
        double velocity_x = 0.0;
        double velocity_y = 0.0;
        double opacity = 1.0;
        double visibility_delay = 0.0;
        double selection_amount = 0.0;
        double selection_velocity = 0.0;
        bool dragging = false;
        bool settling = false;
        bool position_initialized = false;
    };

    void setup_window();
    void setup_ui();
    void apply_display_geometry();
    void schedule_geometry_retry();
    [[nodiscard]] int scale_px(int baseline) const noexcept;
    void sync_mode_chip();
    void set_entry_text_quiet(const std::string& text);
    void refresh_wallpaper();
    void refresh_idle_content();
    void clear_launcher_icon_cache();
    [[nodiscard]] GdkTexture* launcher_icon_texture(
        std::string_view icon_name,
        int logical_pixels
    );
    [[nodiscard]] GtkWidget* make_launcher_icon(
        std::string_view icon_name,
        int logical_pixels
    );
    void rebuild_results();
    GtkListBoxRow* append_result_row(const services::LauncherResult& result);
    void on_search_changed();
    [[nodiscard]] bool parse_clipboard_query(
        std::string_view query,
        std::string& filter
    ) const;
    [[nodiscard]] bool parse_clipboard_clear_query(std::string_view query) const;
    [[nodiscard]] bool parse_emoji_query(
        std::string_view query,
        std::string& filter
    ) const;
    void enter_clipboard_mode(std::string filter);
    void enter_clipboard_clear_mode();
    void leave_clipboard_mode();
    void request_clipboard_history();
    void request_clipboard_wipe();
    void request_clipboard_delete(std::string id);
    void rebuild_clipboard_results();
    bool append_next_clipboard_page(std::size_t minimum_result_count = 0);
    void schedule_clipboard_page_growth();
    void request_clipboard_thumbnail(const services::LauncherResult& result);
    void schedule_visible_clipboard_thumbnails();
    void request_visible_clipboard_thumbnails();
    void ensure_result_row_visible(GtkListBoxRow* row);
    void apply_clipboard_thumbnail(std::string_view id);
    void cache_clipboard_thumbnail(
        std::string id,
        ClipboardThumbnail thumbnail
    );
    void clear_clipboard_thumbnail_cache();
    void activate_clipboard_action(const services::LauncherResult& result);
    void enter_emoji_mode(std::string filter);
    void leave_emoji_mode();
    void request_emoji_database();
    void rebuild_emoji_results();
    bool append_next_emoji_page(std::size_t minimum_result_count = 0);
    void schedule_emoji_page_growth();
    [[nodiscard]] std::string empty_results_message() const;
    void on_result_selected(GtkListBoxRow* row);
    void set_selected_result(const services::LauncherResult* result);
    void retarget_result_selection(GtkListBoxRow* row);
    [[nodiscard]] ResultRowMotion* result_row_motion(GtkListBoxRow* row);
    [[nodiscard]] bool update_result_selection_target();
    void schedule_result_selection_frame();
    [[nodiscard]] bool advance_result_selection_frame(GdkFrameClock* frame_clock);
    void activate_selected();
    void activate_result(std::size_t index);
    bool handle_key(guint keyval, GdkModifierType modifiers);

    void load_constellation_layout();
    void save_constellation_layout() const;
    void seed_constellation_layout();
    void rebuild_constellation();
    void layout_constellation();
    [[nodiscard]] std::pair<double, double> constrain_constellation_position(
        double requested_x,
        double requested_y
    ) const;
    void set_constellation_visible(bool visible);
    void clear_constellation_selection();
    void select_constellation_node(ConstellationNode* node, bool grab_focus);
    void set_hovered_constellation_node(ConstellationNode* node);
    [[nodiscard]] ConstellationNode* active_constellation_highlight() const;
    void retarget_constellation_indicator(bool start_from_search);
    [[nodiscard]] bool navigate_constellation(SpatialDirection direction);
    [[nodiscard]] bool pointer_position_in_constellation(
        GtkEventController* controller,
        double& x,
        double& y
    ) const;
    void schedule_constellation_frame();
    [[nodiscard]] bool advance_constellation_frame(GdkFrameClock* frame_clock);
    void schedule_central_frame();
    [[nodiscard]] bool advance_central_frame(GdkFrameClock* frame_clock);
    void apply_central_motion();
    void apply_central_final_geometry();
    void schedule_central_shader_open();
    [[nodiscard]] bool begin_central_shader(
        bool opening,
        std::string* error = nullptr
    );
    void finish_central_shader();
    void finish_central_hide();
    [[nodiscard]] bool search_query_active() const;
    [[nodiscard]] std::pair<double, double> search_centre_in_constellation() const;
    [[nodiscard]] std::pair<double, double> constellation_emergence_position(
        const ConstellationNode& node
    ) const;
    [[nodiscard]] bool point_hits_constellation_node(double x, double y) const;
    [[nodiscard]] bool constellation_contains(std::string_view application_id) const;
    void pin_constellation_application(std::string_view application_id);
    void unpin_constellation_application(std::string_view application_id);
    void toggle_constellation_application(std::string_view application_id);
    void activate_constellation_node(ConstellationNode& node);
    void begin_constellation_drag(
        ConstellationNode& node,
        GtkEventController* controller,
        double grab_x,
        double grab_y
    );
    void update_constellation_drag(
        ConstellationNode& node,
        double offset_x,
        double offset_y,
        GtkGesture* gesture
    );
    void end_constellation_drag(ConstellationNode& node);
    void show_constellation_menu(ConstellationNode& node, double x, double y);
    [[nodiscard]] std::pair<double, double> default_constellation_position(
        std::size_t index
    ) const;

    GtkWindow* window_ = nullptr;
    GtkWidget* root_ = nullptr;
    GtkWidget* dismiss_ = nullptr;
    GtkWidget* search_entry_ = nullptr;
    GtkWidget* search_slot_ = nullptr;
    GtkWidget* mode_chip_ = nullptr;
    GtkWidget* wallpaper_picture_ = nullptr;
    GtkWidget* wallpaper_shade_ = nullptr;
    GtkWidget* centre_column_ = nullptr;
    GtkWidget* centre_shader_host_ = nullptr;
    GtkWidget* centre_shadow_ = nullptr;
    GtkWidget* centre_effect_view_ = nullptr;
    GtkWidget* centre_shell_ = nullptr;
    GtkWidget* wallpaper_frame_ = nullptr;
    GtkWidget* activation_sweep_ = nullptr;
    GtkWidget* constellation_canvas_ = nullptr;
    GtkWidget* selection_indicator_ = nullptr;
    GtkWidget* results_revealer_ = nullptr;
    GtkWidget* results_shell_ = nullptr;
    GtkWidget* results_overlay_ = nullptr;
    GtkWidget* results_scroller_ = nullptr;
    GtkWidget* results_list_ = nullptr;
    GtkWidget* result_selection_indicator_ = nullptr;

    int monitor_index_ = -1;
    launcher::LauncherGeometry launcher_geometry_{};
    core::DisplayTier display_tier_ = core::DisplayTier::P1080;
    bool geometry_initialized_ = false;
    guint geometry_retry_id_ = 0;

    SearchMode search_mode_ = SearchMode::Normal;
    bool entry_text_programmatic_ = false;
    std::shared_ptr<ClipboardAsyncState> clipboard_async_state_;
    std::string clipboard_history_output_;
    std::string clipboard_filter_;
    std::string clipboard_status_message_;
    std::vector<services::LauncherResult> clipboard_all_results_;
    bool clipboard_history_loaded_ = false;
    bool clipboard_loading_ = false;
    bool clipboard_clear_armed_ = false;
    std::size_t clipboard_rendered_count_ = 0;
    std::uint64_t clipboard_view_generation_ = 0;
    std::unordered_map<std::string, ClipboardRowWidgets> clipboard_rows_;
    std::unordered_map<std::string, ClipboardThumbnail> clipboard_thumbnail_cache_;
    std::vector<std::string> clipboard_thumbnail_lru_;
    std::unordered_map<std::string, std::uint64_t> clipboard_thumbnail_inflight_;
    std::size_t clipboard_thumbnail_active_jobs_ = 0;
    guint clipboard_thumbnail_visibility_idle_id_ = 0;
    guint clipboard_page_growth_idle_id_ = 0;

    std::shared_ptr<EmojiAsyncState> emoji_async_state_;
    std::string emoji_database_text_;
    std::string emoji_filter_;
    std::string emoji_status_message_;
    std::vector<services::LauncherResult> emoji_all_results_;
    bool emoji_database_loaded_ = false;
    bool emoji_loading_ = false;
    std::size_t emoji_rendered_count_ = 0;
    guint emoji_page_growth_idle_id_ = 0;

    services::LauncherService& service_;
    services::WallpaperService& wallpaper_service_;
    CommandReceiptOverlay& command_receipts_;
    std::string wallpaper_texture_path_;
    int wallpaper_texture_scale_factor_ = 0;
    std::unordered_map<std::string, GdkTexture*> launcher_icon_textures_;
    GtkIconTheme* launcher_icon_theme_ = nullptr;
    gulong launcher_icon_theme_changed_handler_ = 0;
    std::vector<services::LauncherResult> current_results_;
    std::optional<services::LauncherResult> selected_result_;
    std::vector<std::unique_ptr<ResultRowMotion>> result_row_motions_;
    std::vector<ConstellationPlacement> constellation_layout_;
    std::vector<std::unique_ptr<ConstellationNode>> constellation_nodes_;
    ConstellationNode* selected_constellation_node_ = nullptr;
    ConstellationNode* hovered_constellation_node_ = nullptr;
    double selection_indicator_x_ = 0.0;
    double selection_indicator_y_ = 0.0;
    double selection_indicator_velocity_x_ = 0.0;
    double selection_indicator_velocity_y_ = 0.0;
    double selection_indicator_opacity_ = 0.0;
    bool selection_indicator_initialized_ = false;
    bool selection_indicator_target_visible_ = false;
    guint constellation_tick_id_ = 0;
    gint64 constellation_last_frame_time_ = 0;
    guint result_selection_tick_id_ = 0;
    gint64 result_selection_last_frame_time_ = 0;
    GtkListBoxRow* selected_result_row_ = nullptr;
    double result_selection_y_ = 0.0;
    double result_selection_velocity_y_ = 0.0;
    double result_selection_height_ = 0.0;
    double result_selection_velocity_height_ = 0.0;
    double result_selection_target_x_ = 0.0;
    double result_selection_target_y_ = 0.0;
    double result_selection_target_width_ = 0.0;
    double result_selection_target_height_ = 0.0;
    double result_selection_opacity_ = 0.0;
    bool result_selection_initialized_ = false;
    bool result_selection_target_visible_ = false;
    double result_pointer_window_x_ = 0.0;
    double result_pointer_window_y_ = 0.0;
    bool result_pointer_position_valid_ = false;
    guint central_tick_id_ = 0;
    guint central_shader_prepare_tick_id_ = 0;
    unsigned int central_shader_prepare_attempts_ = 0;
    gint64 central_last_frame_time_ = 0;
    bool central_shader_preparing_ = false;
    bool central_shader_fallback_ = false;
    std::unique_ptr<effects::shell::ShellShaderRenderer> centre_shader_renderer_;
    effects::TransitionTimeline central_transition_{{0.18, 0.12}};
    bool constellation_layout_loaded_ = false;
    bool constellation_target_visible_ = true;
};

} // namespace realmheart::ui
