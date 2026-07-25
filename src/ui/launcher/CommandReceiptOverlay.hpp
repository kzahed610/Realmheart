#pragma once

#include "services/LauncherService.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <gtk/gtk.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::ui::bar::widgets {
class ThemedSvgIcon;
}

namespace realmheart::ui {

class CommandReceiptOverlay {
public:
    CommandReceiptOverlay();
    ~CommandReceiptOverlay();

    CommandReceiptOverlay(const CommandReceiptOverlay&) = delete;
    CommandReceiptOverlay& operator=(const CommandReceiptOverlay&) = delete;

    // Mounts the receipt inside the launcher's existing fullscreen GtkOverlay.
    // The receipt never creates or presents a second Wayland surface.
    void attach(GtkOverlay* launcher_root);
    void detach();

    // Starts a captured command/action execution and displays its receipt.
    // Returns false only when the result kind is not executable here.
    bool execute(const services::LauncherResult& result);
    void dismiss();

private:
    enum class ReceiptState {
        Running,
        Success,
        Failure,
    };

    struct AsyncState {
        std::atomic<CommandReceiptOverlay*> owner{nullptr};
        std::atomic<std::uint64_t> generation{0};
    };

    struct CompletionPayload {
        std::shared_ptr<AsyncState> state;
        std::uint64_t generation = 0;
        std::chrono::steady_clock::time_point started_at;
    };

    void setup_ui();
    void reset_after_collapse();
    void show_running(
        const services::LauncherResult& result,
        std::string display_command
    );
    void complete_execution(
        std::uint64_t generation,
        bool successful,
        int exit_code,
        double duration_seconds,
        std::string standard_output,
        std::string standard_error,
        std::string launch_error
    );
    void set_state(ReceiptState state);
    void set_logs_expanded(bool expanded);
    void update_log_view();
    void copy_logs() const;
    void retry();
    void schedule_auto_dismiss();
    void cancel_auto_dismiss();
    void present_receipt();
    void begin_drag(
        GtkGestureDrag* gesture,
        double start_x,
        double start_y
    );
    void update_drag(GtkEventController* controller, double fallback_offset_y);
    void end_drag();
    [[nodiscard]] bool pointer_y_in_surface(
        GtkEventController* controller,
        double& y
    ) const;
    void position_trash_target();
    void set_trash_armed(bool armed);
    void schedule_drag_frame();
    [[nodiscard]] bool advance_drag_frame(GdkFrameClock* frame_clock);
    void apply_drag_offset();
    void reset_drag_visuals();

    [[nodiscard]] std::vector<std::string> execution_argv(
        const services::LauncherResult& result
    ) const;
    [[nodiscard]] std::string display_command_for(
        const services::LauncherResult& result
    ) const;
    [[nodiscard]] std::string compact_output_summary(
        bool successful,
        std::string_view standard_output,
        std::string_view standard_error,
        std::string_view launch_error
    ) const;

    static void communicate_finished(
        GObject* source_object,
        GAsyncResult* result,
        gpointer user_data
    );
    static gboolean auto_dismiss_timeout(gpointer user_data);
    static gboolean drag_tick(
        GtkWidget* widget,
        GdkFrameClock* frame_clock,
        gpointer user_data
    );

    GtkOverlay* launcher_root_ = nullptr;
    GtkWidget* reveal_ = nullptr;
    GtkWidget* trash_revealer_ = nullptr;
    GtkWidget* trash_target_ = nullptr;
    std::unique_ptr<bar::widgets::ThemedSvgIcon> trash_icon_;
    GtkWidget* card_ = nullptr;
    GtkWidget* status_glyph_ = nullptr;
    GtkWidget* title_label_ = nullptr;
    GtkWidget* state_label_ = nullptr;
    GtkWidget* command_label_ = nullptr;
    GtkWidget* summary_label_ = nullptr;
    GtkWidget* metadata_label_ = nullptr;
    GtkWidget* close_button_ = nullptr;
    GtkWidget* full_logs_button_ = nullptr;
    GtkWidget* retry_button_ = nullptr;
    GtkWidget* copy_button_ = nullptr;
    GtkWidget* logs_revealer_ = nullptr;
    GtkWidget* log_view_ = nullptr;

    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
    GSubprocess* active_process_ = nullptr;
    std::optional<services::LauncherResult> current_result_;
    std::string current_command_;
    std::string standard_output_;
    std::string standard_error_;
    std::string launch_error_;
    ReceiptState state_ = ReceiptState::Running;
    guint auto_dismiss_id_ = 0;
    guint drag_tick_id_ = 0;
    gint64 drag_last_frame_time_ = 0;
    std::uint64_t dismissed_generation_ = 0;
    int exit_code_ = 0;
    double duration_seconds_ = 0.0;
    bool logs_expanded_ = false;
    bool pointer_inside_ = false;
    bool closing_ = false;
    bool dragging_receipt_ = false;
    bool drag_blocked_ = false;
    bool trash_armed_ = false;
    bool drag_dismiss_pending_ = false;
    double drag_raw_y_ = 0.0;
    double drag_offset_y_ = 0.0;
    double drag_target_y_ = 0.0;
    double drag_velocity_y_ = 0.0;
    double drag_dismiss_start_y_ = 0.0;
    double drag_pointer_start_y_ = 0.0;
    bool drag_pointer_has_surface_origin_ = false;
    int trash_margin_top_ = 0;
};

} // namespace realmheart::ui
