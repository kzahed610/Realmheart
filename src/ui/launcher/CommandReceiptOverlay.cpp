#include "ui/launcher/CommandReceiptOverlay.hpp"

#include "core/Command.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace realmheart::ui {
namespace {

constexpr int kCompactWidth = 390;
constexpr int kReceiptTopMargin = 172;
constexpr int kReceiptRightMargin = 56;
constexpr int kTrashTargetSize = 112;
constexpr int kTrashIconSize = 40;
constexpr int kTrashTargetGap = 44;
constexpr double kDragDeadzone = 3.0;
constexpr double kMinimumDismissPointerTravel = 34.0;
constexpr double kDiscardTravel = 92.0;
constexpr double kDragSpringStiffness = 245.0;
constexpr double kDragSpringDamping = 26.0;
constexpr int kSuccessDismissMs = 5000;
constexpr int kFailureDismissMs = 11000;
constexpr std::size_t kSummaryMaximumCharacters = 320;
constexpr std::size_t kSummaryMaximumLines = 4;
constexpr std::size_t kMaximumCapturedOutputBytes = 512 * 1024;

std::string trim_copy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\n\r");
    return std::string(value.substr(first, last - first + 1));
}

std::vector<std::string> nonempty_lines(std::string_view value) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find('\n', start);
        const std::string line = trim_copy(value.substr(
            start,
            end == std::string_view::npos ? value.size() - start : end - start
        ));
        if (!line.empty()) lines.push_back(line);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return lines;
}

std::string tail_excerpt(std::string_view value) {
    const auto lines = nonempty_lines(value);
    if (lines.empty()) return {};

    const std::size_t first = lines.size() > kSummaryMaximumLines
        ? lines.size() - kSummaryMaximumLines
        : 0;
    std::string excerpt;
    for (std::size_t index = first; index < lines.size(); ++index) {
        if (!excerpt.empty()) excerpt.push_back('\n');
        excerpt += lines[index];
        if (excerpt.size() >= kSummaryMaximumCharacters) break;
    }
    if (excerpt.size() > kSummaryMaximumCharacters) {
        excerpt.resize(kSummaryMaximumCharacters - 1);
        excerpt += "…";
    }
    return excerpt;
}


std::string bounded_output(std::string value) {
    if (value.size() <= kMaximumCapturedOutputBytes) return value;

    constexpr std::string_view marker =
        "\n\n… Realmheart truncated the middle of this output …\n\n";
    const std::size_t remaining = kMaximumCapturedOutputBytes - marker.size();
    const std::size_t prefix_size = remaining / 3;
    const std::size_t suffix_size = remaining - prefix_size;
    return value.substr(0, prefix_size) + std::string(marker) +
        value.substr(value.size() - suffix_size);
}

std::string duration_text(double seconds) {
    std::ostringstream stream;
    if (seconds < 10.0) {
        stream << std::fixed << std::setprecision(2) << seconds << 's';
    } else {
        stream << std::fixed << std::setprecision(1) << seconds << 's';
    }
    return stream.str();
}

std::string shell_quote(std::string_view value) {
    std::string quoted{"'"};
    for (const char character : value) {
        if (character == '\'') quoted += "'\\''";
        else quoted.push_back(character);
    }
    quoted.push_back('\'');
    return quoted;
}

void remove_state_classes(GtkWidget* widget) {
    gtk_widget_remove_css_class(widget, "running");
    gtk_widget_remove_css_class(widget, "success");
    gtk_widget_remove_css_class(widget, "failure");
}

bool is_inside_button(GtkWidget* widget, GtkWidget* stop_at) {
    for (GtkWidget* current = widget;
         current != nullptr && current != stop_at;
         current = gtk_widget_get_parent(current)) {
        if (GTK_IS_BUTTON(current)) return true;
    }
    return false;
}

} // namespace

CommandReceiptOverlay::CommandReceiptOverlay() {
    async_state_->owner.store(this);
}

CommandReceiptOverlay::~CommandReceiptOverlay() {
    async_state_->owner.store(nullptr);
    async_state_->generation.fetch_add(1);
    cancel_auto_dismiss();
    if (drag_tick_id_ != 0 && reveal_ != nullptr) {
        gtk_widget_remove_tick_callback(reveal_, drag_tick_id_);
        drag_tick_id_ = 0;
    }
    if (active_process_ != nullptr) {
        g_object_unref(active_process_);
        active_process_ = nullptr;
    }
    detach();
    if (reveal_ != nullptr) {
        g_object_unref(reveal_);
        reveal_ = nullptr;
    }
    if (trash_revealer_ != nullptr) {
        g_object_unref(trash_revealer_);
        trash_revealer_ = nullptr;
    }
}

void CommandReceiptOverlay::attach(GtkOverlay* launcher_root) {
    if (launcher_root == nullptr || launcher_root_ == launcher_root) return;
    detach();
    launcher_root_ = launcher_root;
    if (reveal_ == nullptr) setup_ui();

    gtk_widget_set_halign(reveal_, GTK_ALIGN_END);
    gtk_widget_set_valign(reveal_, GTK_ALIGN_START);
    // Keep the receipt clear of both the top edge and the right bezel.
    // This is deliberately lower than the launcher's previous position.
    gtk_widget_set_margin_top(reveal_, kReceiptTopMargin);
    gtk_widget_set_margin_end(reveal_, kReceiptRightMargin);
    gtk_widget_set_hexpand(reveal_, FALSE);
    gtk_widget_set_vexpand(reveal_, FALSE);
    gtk_widget_set_visible(reveal_, FALSE);

    gtk_widget_set_halign(trash_revealer_, GTK_ALIGN_END);
    gtk_widget_set_valign(trash_revealer_, GTK_ALIGN_START);
    gtk_widget_set_margin_end(
        trash_revealer_,
        kReceiptRightMargin + (kCompactWidth - kTrashTargetSize) / 2
    );
    gtk_widget_set_hexpand(trash_revealer_, FALSE);
    gtk_widget_set_vexpand(trash_revealer_, FALSE);
    gtk_widget_set_visible(trash_revealer_, FALSE);
    gtk_overlay_add_overlay(launcher_root_, trash_revealer_);
    gtk_overlay_set_measure_overlay(launcher_root_, trash_revealer_, FALSE);
    gtk_overlay_set_clip_overlay(launcher_root_, trash_revealer_, FALSE);

    gtk_overlay_add_overlay(launcher_root_, reveal_);
    gtk_overlay_set_measure_overlay(launcher_root_, reveal_, FALSE);
    gtk_overlay_set_clip_overlay(launcher_root_, reveal_, FALSE);
}

void CommandReceiptOverlay::detach() {
    launcher_root_ = nullptr;
    if (drag_tick_id_ != 0 && reveal_ != nullptr) {
        gtk_widget_remove_tick_callback(reveal_, drag_tick_id_);
        drag_tick_id_ = 0;
    }
    if (trash_revealer_ != nullptr &&
        gtk_widget_get_parent(trash_revealer_) != nullptr) {
        gtk_widget_unparent(trash_revealer_);
    }
    if (reveal_ != nullptr && gtk_widget_get_parent(reveal_) != nullptr) {
        gtk_widget_unparent(reveal_);
    }
}

void CommandReceiptOverlay::setup_ui() {
    trash_revealer_ = gtk_revealer_new();
    g_object_ref_sink(trash_revealer_);
    gtk_revealer_set_transition_type(
        GTK_REVEALER(trash_revealer_),
        GTK_REVEALER_TRANSITION_TYPE_CROSSFADE
    );
    gtk_revealer_set_transition_duration(GTK_REVEALER(trash_revealer_), 130);
    gtk_widget_add_css_class(
        trash_revealer_,
        "realmheart-command-receipt-trash-host"
    );
    gtk_widget_set_visible(trash_revealer_, FALSE);

    // A centre box is used deliberately here. A one-child vertical GtkBox
    // places the icon at the start of its main axis when the target is larger
    // than the icon, which made the placeholder look visibly off-centre.
    trash_target_ = gtk_center_box_new();
    gtk_widget_set_size_request(
        trash_target_,
        kTrashTargetSize,
        kTrashTargetSize
    );
    gtk_widget_set_halign(trash_target_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(trash_target_, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(trash_target_, FALSE);
    gtk_widget_add_css_class(
        trash_target_,
        "realmheart-command-receipt-trash-target"
    );
    trash_icon_ = std::make_unique<bar::widgets::ThemedSvgIcon>(
        "Realmheart-Icons/trash.svg",
        kTrashIconSize
    );
    trash_icon_->add_css_class("realmheart-command-receipt-trash-icon");
    GtkWidget* trash_icon_widget = trash_icon_->widget();
    gtk_widget_set_halign(trash_icon_widget, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(trash_icon_widget, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(trash_icon_widget, FALSE);
    gtk_widget_set_vexpand(trash_icon_widget, FALSE);
    gtk_center_box_set_center_widget(
        GTK_CENTER_BOX(trash_target_),
        trash_icon_widget
    );
    gtk_revealer_set_child(GTK_REVEALER(trash_revealer_), trash_target_);

    reveal_ = gtk_revealer_new();
    // Keep one owner reference so detaching from the launcher does not destroy
    // the widget before ShellRuntime tears down the receipt controller.
    g_object_ref_sink(reveal_);
    gtk_revealer_set_transition_type(
        GTK_REVEALER(reveal_),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN
    );
    gtk_revealer_set_transition_duration(GTK_REVEALER(reveal_), 210);
    gtk_widget_add_css_class(reveal_, "realmheart-command-receipt-host");
    gtk_widget_set_visible(reveal_, FALSE);
    g_signal_connect(reveal_, "notify::child-revealed", G_CALLBACK(+[](
        GObject* object, GParamSpec*, gpointer data
    ) {
        auto* self = static_cast<CommandReceiptOverlay*>(data);
        if (!gtk_revealer_get_child_revealed(GTK_REVEALER(object)) &&
            self->closing_) {
            self->reset_after_collapse();
        }
    }), this);

    card_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(card_, kCompactWidth, -1);
    gtk_widget_add_css_class(card_, "realmheart-command-receipt-card");

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(header, "realmheart-command-receipt-header");

    GtkWidget* drag_handle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_hexpand(drag_handle, TRUE);
    gtk_widget_add_css_class(
        drag_handle,
        "realmheart-command-receipt-drag-handle"
    );

    status_glyph_ = gtk_label_new("◌");
    gtk_widget_add_css_class(
        status_glyph_,
        "realmheart-command-receipt-status-glyph"
    );
    gtk_box_append(GTK_BOX(drag_handle), status_glyph_);

    GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(heading, TRUE);
    title_label_ = gtk_label_new("Command");
    gtk_label_set_xalign(GTK_LABEL(title_label_), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(title_label_), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(
        title_label_,
        "realmheart-command-receipt-title"
    );
    state_label_ = gtk_label_new("Running");
    gtk_label_set_xalign(GTK_LABEL(state_label_), 0.0F);
    gtk_widget_add_css_class(
        state_label_,
        "realmheart-command-receipt-state"
    );
    gtk_box_append(GTK_BOX(heading), title_label_);
    gtk_box_append(GTK_BOX(heading), state_label_);
    gtk_box_append(GTK_BOX(drag_handle), heading);
    gtk_box_append(GTK_BOX(header), drag_handle);

    close_button_ = gtk_button_new_with_label("×");
    gtk_button_set_has_frame(GTK_BUTTON(close_button_), FALSE);
    gtk_widget_set_tooltip_text(close_button_, "Close command receipt");
    gtk_widget_add_css_class(
        close_button_,
        "realmheart-command-receipt-close"
    );
    g_signal_connect(
        close_button_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<CommandReceiptOverlay*>(data)->dismiss();
        }),
        this
    );
    gtk_box_append(GTK_BOX(header), close_button_);
    gtk_box_append(GTK_BOX(card_), header);

    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    // Capture on the whole receipt so any non-button surface can initiate the
    // gesture. Button descendants are explicitly denied in begin_drag(), which
    // preserves their normal click behaviour.
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(drag),
        GTK_PHASE_CAPTURE
    );
    g_signal_connect(
        drag,
        "drag-begin",
        G_CALLBACK(+[](
            GtkGestureDrag* gesture,
            double start_x,
            double start_y,
            gpointer data
        ) {
            static_cast<CommandReceiptOverlay*>(data)->begin_drag(
                gesture,
                start_x,
                start_y
            );
        }),
        this
    );
    g_signal_connect(
        drag,
        "drag-update",
        G_CALLBACK(+[](
            GtkGestureDrag* gesture,
            double,
            double offset_y,
            gpointer data
        ) {
            static_cast<CommandReceiptOverlay*>(data)->update_drag(
                GTK_EVENT_CONTROLLER(gesture),
                offset_y
            );
        }),
        this
    );
    g_signal_connect(
        drag,
        "drag-end",
        G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer data) {
            static_cast<CommandReceiptOverlay*>(data)->end_drag();
        }),
        this
    );
    g_signal_connect(
        drag,
        "cancel",
        G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
            static_cast<CommandReceiptOverlay*>(data)->end_drag();
        }),
        this
    );
    gtk_widget_add_controller(card_, GTK_EVENT_CONTROLLER(drag));

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_add_css_class(body, "realmheart-command-receipt-body");

    command_label_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(command_label_), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(command_label_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(
        command_label_,
        "realmheart-command-receipt-command"
    );
    gtk_box_append(GTK_BOX(body), command_label_);

    summary_label_ = gtk_label_new("Waiting for command output…");
    gtk_label_set_xalign(GTK_LABEL(summary_label_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(summary_label_), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(summary_label_), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(summary_label_), 52);
    gtk_widget_add_css_class(
        summary_label_,
        "realmheart-command-receipt-summary"
    );
    gtk_box_append(GTK_BOX(body), summary_label_);

    metadata_label_ = gtk_label_new("Running");
    gtk_label_set_xalign(GTK_LABEL(metadata_label_), 0.0F);
    gtk_widget_add_css_class(
        metadata_label_,
        "realmheart-command-receipt-metadata"
    );
    gtk_box_append(GTK_BOX(body), metadata_label_);
    gtk_box_append(GTK_BOX(card_), body);

    logs_revealer_ = gtk_revealer_new();
    gtk_revealer_set_transition_type(
        GTK_REVEALER(logs_revealer_),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN
    );
    gtk_revealer_set_transition_duration(GTK_REVEALER(logs_revealer_), 230);
    gtk_widget_set_hexpand(logs_revealer_, TRUE);
    gtk_widget_set_vexpand(logs_revealer_, FALSE);

    GtkWidget* log_shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(log_shell, "realmheart-command-receipt-log-shell");
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_AUTOMATIC,
        GTK_POLICY_AUTOMATIC
    );
    gtk_widget_set_size_request(scroller, -1, 260);
    gtk_widget_add_css_class(
        scroller,
        "realmheart-command-receipt-log-scroller"
    );

    log_view_ = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view_), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view_), TRUE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view_), GTK_WRAP_NONE);
    gtk_widget_add_css_class(
        log_view_,
        "realmheart-command-receipt-log-view"
    );
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), log_view_);
    gtk_box_append(GTK_BOX(log_shell), scroller);
    gtk_revealer_set_child(GTK_REVEALER(logs_revealer_), log_shell);
    gtk_box_append(GTK_BOX(card_), logs_revealer_);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_widget_add_css_class(actions, "realmheart-command-receipt-actions");

    copy_button_ = gtk_button_new_with_label("Copy");
    gtk_widget_add_css_class(copy_button_, "realmheart-command-receipt-action");
    g_signal_connect(
        copy_button_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<CommandReceiptOverlay*>(data)->copy_logs();
        }),
        this
    );
    gtk_box_append(GTK_BOX(actions), copy_button_);

    retry_button_ = gtk_button_new_with_label("Retry");
    gtk_widget_add_css_class(retry_button_, "realmheart-command-receipt-action");
    g_signal_connect(
        retry_button_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            static_cast<CommandReceiptOverlay*>(data)->retry();
        }),
        this
    );
    gtk_box_append(GTK_BOX(actions), retry_button_);

    full_logs_button_ = gtk_button_new_with_label("Full logs");
    gtk_widget_add_css_class(
        full_logs_button_,
        "realmheart-command-receipt-action"
    );
    gtk_widget_add_css_class(
        full_logs_button_,
        "suggested-action"
    );
    g_signal_connect(
        full_logs_button_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<CommandReceiptOverlay*>(data);
            self->set_logs_expanded(!self->logs_expanded_);
        }),
        this
    );
    gtk_box_append(GTK_BOX(actions), full_logs_button_);
    gtk_box_append(GTK_BOX(card_), actions);

    GtkEventController* hover = gtk_event_controller_motion_new();
    g_signal_connect(
        hover,
        "enter",
        G_CALLBACK(+[](GtkEventControllerMotion*, double, double, gpointer data) {
            auto* self = static_cast<CommandReceiptOverlay*>(data);
            self->pointer_inside_ = true;
            self->cancel_auto_dismiss();
        }),
        this
    );
    g_signal_connect(
        hover,
        "leave",
        G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
            auto* self = static_cast<CommandReceiptOverlay*>(data);
            self->pointer_inside_ = false;
            self->schedule_auto_dismiss();
        }),
        this
    );
    gtk_widget_add_controller(card_, hover);

    gtk_revealer_set_child(GTK_REVEALER(reveal_), card_);
}

bool CommandReceiptOverlay::execute(const services::LauncherResult& result) {
    if (launcher_root_ == nullptr || reveal_ == nullptr) return false;
    if (result.kind != services::LauncherResultKind::Command &&
        result.kind != services::LauncherResultKind::Action) {
        return false;
    }

    const std::vector<std::string> argv = execution_argv(result);
    if (argv.empty()) return false;

    const std::uint64_t generation = async_state_->generation.fetch_add(1) + 1;
    reset_drag_visuals();
    dismissed_generation_ = 0;
    closing_ = false;
    current_result_ = result;
    current_command_ = display_command_for(result);
    standard_output_.clear();
    standard_error_.clear();
    launch_error_.clear();
    exit_code_ = 0;
    duration_seconds_ = 0.0;
    set_logs_expanded(false);
    show_running(result, current_command_);
    present_receipt();

    GSubprocessFlags flags = static_cast<GSubprocessFlags>(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE
    );
    GSubprocessLauncher* launcher = g_subprocess_launcher_new(flags);
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        g_subprocess_launcher_set_cwd(launcher, home);
    }

    std::vector<const gchar*> arguments;
    arguments.reserve(argv.size() + 1);
    for (const auto& argument : argv) arguments.push_back(argument.c_str());
    arguments.push_back(nullptr);

    GError* error = nullptr;
    GSubprocess* process = g_subprocess_launcher_spawnv(
        launcher,
        arguments.data(),
        &error
    );
    g_object_unref(launcher);

    if (process == nullptr) {
        const std::string message = error != nullptr && error->message != nullptr
            ? error->message
            : "Unable to start command";
        if (error != nullptr) g_error_free(error);
        complete_execution(generation, false, -1, 0.0, {}, {}, message);
        return true;
    }

    if (active_process_ != nullptr) g_object_unref(active_process_);
    active_process_ = G_SUBPROCESS(g_object_ref(process));

    auto* payload = new CompletionPayload{
        async_state_,
        generation,
        std::chrono::steady_clock::now(),
    };
    g_subprocess_communicate_utf8_async(
        process,
        nullptr,
        nullptr,
        &CommandReceiptOverlay::communicate_finished,
        payload
    );
    g_object_unref(process);
    return true;
}

void CommandReceiptOverlay::show_running(
    const services::LauncherResult& result,
    std::string display_command
) {
    set_state(ReceiptState::Running);
    gtk_label_set_text(
        GTK_LABEL(title_label_),
        result.kind == services::LauncherResultKind::Action
            ? result.title.c_str()
            : "Command"
    );
    gtk_label_set_text(GTK_LABEL(state_label_), "Running");
    gtk_label_set_text(GTK_LABEL(command_label_), display_command.c_str());
    gtk_label_set_text(GTK_LABEL(summary_label_), "Waiting for command output…");
    gtk_label_set_text(GTK_LABEL(metadata_label_), "Process is running");
    gtk_widget_set_sensitive(full_logs_button_, FALSE);
    gtk_widget_set_sensitive(copy_button_, FALSE);
    gtk_widget_set_visible(retry_button_, FALSE);
    cancel_auto_dismiss();
}

void CommandReceiptOverlay::complete_execution(
    std::uint64_t generation,
    bool successful,
    int exit_code,
    double duration_seconds,
    std::string standard_output,
    std::string standard_error,
    std::string launch_error
) {
    if (generation != async_state_->generation.load()) return;

    if (active_process_ != nullptr) {
        g_object_unref(active_process_);
        active_process_ = nullptr;
    }

    standard_output_ = std::move(standard_output);
    standard_error_ = std::move(standard_error);
    launch_error_ = std::move(launch_error);
    exit_code_ = exit_code;
    duration_seconds_ = duration_seconds;
    set_state(successful ? ReceiptState::Success : ReceiptState::Failure);

    gtk_label_set_text(
        GTK_LABEL(state_label_),
        successful ? "Completed" : "Command failed"
    );
    const std::string summary = compact_output_summary(
        successful,
        standard_output_,
        standard_error_,
        launch_error_
    );
    gtk_label_set_text(GTK_LABEL(summary_label_), summary.c_str());

    std::string metadata = exit_code >= 0
        ? "Exit " + std::to_string(exit_code)
        : "Unable to read exit status";
    metadata += "  ·  " + duration_text(duration_seconds_);
    gtk_label_set_text(GTK_LABEL(metadata_label_), metadata.c_str());

    gtk_widget_set_sensitive(full_logs_button_, TRUE);
    gtk_widget_set_sensitive(copy_button_, TRUE);
    gtk_widget_set_visible(retry_button_, !successful);
    update_log_view();

    if (dismissed_generation_ != generation) {
        present_receipt();
        schedule_auto_dismiss();
    }
}

void CommandReceiptOverlay::set_state(ReceiptState state) {
    state_ = state;
    remove_state_classes(card_);
    switch (state_) {
    case ReceiptState::Running:
        gtk_widget_add_css_class(card_, "running");
        gtk_label_set_text(GTK_LABEL(status_glyph_), "◌");
        break;
    case ReceiptState::Success:
        gtk_widget_add_css_class(card_, "success");
        gtk_label_set_text(GTK_LABEL(status_glyph_), "✓");
        break;
    case ReceiptState::Failure:
        gtk_widget_add_css_class(card_, "failure");
        gtk_label_set_text(GTK_LABEL(status_glyph_), "!");
        break;
    }
}

void CommandReceiptOverlay::set_logs_expanded(bool expanded) {
    logs_expanded_ = expanded;
    if (logs_revealer_ == nullptr) return;

    gtk_revealer_set_reveal_child(GTK_REVEALER(logs_revealer_), expanded);
    gtk_button_set_label(
        GTK_BUTTON(full_logs_button_),
        expanded ? "Collapse" : "Full logs"
    );
    // The receipt is anchored to the upper-right and keeps one stable
    // width. Full logs reveal only downward; horizontal resizing made
    // the card feel like an unrelated window rather than an unfurling
    // execution receipt.
    gtk_widget_set_size_request(card_, kCompactWidth, -1);

    if (expanded) cancel_auto_dismiss();
    else schedule_auto_dismiss();
}

void CommandReceiptOverlay::update_log_view() {
    std::ostringstream log;
    log << "$ " << current_command_ << "\n\n";
    if (!standard_output_.empty()) {
        log << "stdout\n------\n" << standard_output_;
        if (standard_output_.back() != '\n') log << '\n';
        log << '\n';
    }
    if (!standard_error_.empty()) {
        log << "stderr\n------\n" << standard_error_;
        if (standard_error_.back() != '\n') log << '\n';
        log << '\n';
    }
    if (!launch_error_.empty()) {
        log << "launcher error\n--------------\n" << launch_error_ << "\n\n";
    }
    if (standard_output_.empty() && standard_error_.empty() && launch_error_.empty()) {
        log << "No output was captured.\n";
    }
    if (state_ != ReceiptState::Running) {
        log << "Exit code: " << exit_code_ << '\n';
        log << "Duration: " << duration_text(duration_seconds_) << '\n';
    }

    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view_));
    const std::string text = log.str();
    gtk_text_buffer_set_text(buffer, text.c_str(), static_cast<int>(text.size()));
}

void CommandReceiptOverlay::copy_logs() const {
    if (log_view_ == nullptr) return;
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view_));
    GtkTextIter start{};
    GtkTextIter end{};
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    if (text == nullptr) return;

    GdkClipboard* clipboard = gdk_display_get_clipboard(
        gtk_widget_get_display(card_)
    );
    if (clipboard != nullptr) gdk_clipboard_set_text(clipboard, text);
    g_free(text);
}

void CommandReceiptOverlay::retry() {
    if (!current_result_) return;
    const services::LauncherResult result = *current_result_;
    static_cast<void>(execute(result));
}

void CommandReceiptOverlay::schedule_auto_dismiss() {
    cancel_auto_dismiss();
    if (closing_ || pointer_inside_ || logs_expanded_ ||
        dragging_receipt_ || drag_dismiss_pending_ ||
        state_ == ReceiptState::Running) {
        return;
    }

    const int timeout = state_ == ReceiptState::Failure
        ? kFailureDismissMs
        : kSuccessDismissMs;
    auto_dismiss_id_ = g_timeout_add(
        static_cast<guint>(timeout),
        &CommandReceiptOverlay::auto_dismiss_timeout,
        this
    );
}

void CommandReceiptOverlay::cancel_auto_dismiss() {
    if (auto_dismiss_id_ == 0) return;
    g_source_remove(auto_dismiss_id_);
    auto_dismiss_id_ = 0;
}

void CommandReceiptOverlay::present_receipt() {
    if (launcher_root_ == nullptr || reveal_ == nullptr) return;
    if (!dragging_receipt_ && !drag_dismiss_pending_) {
        drag_offset_y_ = 0.0;
        drag_target_y_ = 0.0;
        drag_velocity_y_ = 0.0;
        apply_drag_offset();
        gtk_widget_set_opacity(card_, 1.0);
    }
    closing_ = false;
    gtk_widget_set_visible(reveal_, TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(reveal_), TRUE);
}

void CommandReceiptOverlay::dismiss() {
    if (reveal_ == nullptr) return;
    cancel_auto_dismiss();
    dismissed_generation_ = async_state_->generation.load();
    closing_ = true;
    // Do not collapse the nested full-log revealer while the outer receipt is
    // still folding. Two simultaneous size animations caused the old card to
    // tear and jump on Wayland. The hidden-state cleanup happens only after the
    // outer reveal has completed.
    gtk_revealer_set_reveal_child(GTK_REVEALER(reveal_), FALSE);
}

void CommandReceiptOverlay::reset_after_collapse() {
    if (reveal_ == nullptr) return;
    gtk_widget_set_visible(reveal_, FALSE);

    if (logs_expanded_ && logs_revealer_ != nullptr) {
        const guint duration = gtk_revealer_get_transition_duration(
            GTK_REVEALER(logs_revealer_)
        );
        gtk_revealer_set_transition_duration(GTK_REVEALER(logs_revealer_), 0);
        gtk_revealer_set_reveal_child(GTK_REVEALER(logs_revealer_), FALSE);
        gtk_revealer_set_transition_duration(
            GTK_REVEALER(logs_revealer_),
            duration
        );
    }
    logs_expanded_ = false;
    gtk_button_set_label(GTK_BUTTON(full_logs_button_), "Full logs");
    reset_drag_visuals();
    closing_ = false;
}

bool CommandReceiptOverlay::pointer_y_in_surface(
    GtkEventController* controller,
    double& y
) const {
    if (controller == nullptr) return false;
    GdkEvent* event = gtk_event_controller_get_current_event(controller);
    if (event == nullptr) return false;

    double x = 0.0;
    double surface_y = 0.0;
    if (!gdk_event_get_position(event, &x, &surface_y)) return false;
    y = surface_y;
    return true;
}

void CommandReceiptOverlay::begin_drag(
    GtkGestureDrag* gesture,
    double start_x,
    double start_y
) {
    drag_blocked_ = false;
    if (closing_ || drag_dismiss_pending_ || reveal_ == nullptr ||
        card_ == nullptr) {
        drag_blocked_ = true;
        gtk_gesture_set_state(
            GTK_GESTURE(gesture),
            GTK_EVENT_SEQUENCE_DENIED
        );
        return;
    }

    GtkWidget* picked = gtk_widget_pick(
        card_,
        start_x,
        start_y,
        GTK_PICK_DEFAULT
    );
    if (is_inside_button(picked, card_)) {
        drag_blocked_ = true;
        gtk_gesture_set_state(
            GTK_GESTURE(gesture),
            GTK_EVENT_SEQUENCE_DENIED
        );
        return;
    }

    cancel_auto_dismiss();
    if (drag_tick_id_ != 0) {
        gtk_widget_remove_tick_callback(reveal_, drag_tick_id_);
        drag_tick_id_ = 0;
    }
    dragging_receipt_ = true;
    drag_dismiss_pending_ = false;
    drag_raw_y_ = 0.0;
    drag_target_y_ = drag_offset_y_;
    drag_velocity_y_ = 0.0;
    drag_last_frame_time_ = 0;
    drag_pointer_start_y_ = 0.0;
    drag_pointer_has_surface_origin_ = pointer_y_in_surface(
        GTK_EVENT_CONTROLLER(gesture),
        drag_pointer_start_y_
    );
    position_trash_target();
    set_trash_armed(false);
}

void CommandReceiptOverlay::update_drag(
    GtkEventController* controller,
    double fallback_offset_y
) {
    if (drag_blocked_ || !dragging_receipt_ || reveal_ == nullptr ||
        card_ == nullptr) {
        return;
    }

    double raw_offset_y = fallback_offset_y;
    double pointer_y = 0.0;
    if (drag_pointer_has_surface_origin_ &&
        pointer_y_in_surface(controller, pointer_y)) {
        // Surface coordinates remain stable while the receipt moves. Gesture
        // offsets are local to the moving drag handle and therefore feed the
        // receipt's own motion back into the next event, producing jitter.
        raw_offset_y = pointer_y - drag_pointer_start_y_;
    }
    drag_raw_y_ = raw_offset_y;

    if (std::abs(raw_offset_y) > kDragDeadzone && trash_revealer_ != nullptr) {
        gtk_gesture_set_state(
            GTK_GESTURE(controller),
            GTK_EVENT_SEQUENCE_CLAIMED
        );
        gtk_widget_add_css_class(card_, "dragging");
        gtk_widget_set_visible(trash_revealer_, TRUE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(trash_revealer_), TRUE);
    }

    if (raw_offset_y <= 0.0) {
        drag_target_y_ = raw_offset_y * 0.12;
    } else {
        const double effective = std::max(0.0, raw_offset_y - kDragDeadzone);
        const double magnetic_pull =
            16.0 * (1.0 - std::exp(-effective / 32.0));
        drag_target_y_ = effective * 1.15 + magnetic_pull;
    }

    // Pointer events only retarget the spring. Actual widget movement is
    // coalesced to the compositor frame clock, eliminating event-rate jitter.
    schedule_drag_frame();
}

void CommandReceiptOverlay::end_drag() {
    if (drag_blocked_) {
        drag_blocked_ = false;
        return;
    }
    if (!dragging_receipt_) return;
    dragging_receipt_ = false;
    gtk_widget_remove_css_class(card_, "dragging");

    if (trash_armed_ && drag_raw_y_ >= kMinimumDismissPointerTravel) {
        drag_dismiss_pending_ = true;
        drag_dismiss_start_y_ = drag_offset_y_;
        drag_target_y_ = drag_offset_y_ + kDiscardTravel;
        drag_velocity_y_ = std::max(180.0, drag_velocity_y_);
        gtk_widget_add_css_class(card_, "discarding");
    } else {
        drag_target_y_ = 0.0;
    }
    drag_last_frame_time_ = 0;
    schedule_drag_frame();
}

void CommandReceiptOverlay::position_trash_target() {
    if (launcher_root_ == nullptr || trash_revealer_ == nullptr || card_ == nullptr) {
        return;
    }

    const int card_height = std::max(1, gtk_widget_get_height(card_));
    const int root_height = std::max(1, gtk_widget_get_height(GTK_WIDGET(launcher_root_)));
    const int desired_top = kReceiptTopMargin + card_height + kTrashTargetGap;
    const int maximum_top = std::max(
        0,
        root_height - kTrashTargetSize - 28
    );
    trash_margin_top_ = std::min(desired_top, maximum_top);
    gtk_widget_set_margin_top(trash_revealer_, trash_margin_top_);
}

void CommandReceiptOverlay::set_trash_armed(bool armed) {
    if (trash_armed_ == armed) return;
    trash_armed_ = armed;
    if (trash_target_ != nullptr) {
        if (armed) gtk_widget_add_css_class(trash_target_, "armed");
        else gtk_widget_remove_css_class(trash_target_, "armed");
    }
    if (card_ != nullptr) {
        if (armed) gtk_widget_add_css_class(card_, "dismiss-armed");
        else gtk_widget_remove_css_class(card_, "dismiss-armed");
    }
}

void CommandReceiptOverlay::schedule_drag_frame() {
    if (reveal_ == nullptr || drag_tick_id_ != 0) return;
    drag_tick_id_ = gtk_widget_add_tick_callback(
        reveal_,
        &CommandReceiptOverlay::drag_tick,
        this,
        nullptr
    );
}

bool CommandReceiptOverlay::advance_drag_frame(GdkFrameClock* frame_clock) {
    if (reveal_ == nullptr || card_ == nullptr) return false;

    const gint64 frame_time = gdk_frame_clock_get_frame_time(frame_clock);
    double delta_seconds = 1.0 / 60.0;
    if (drag_last_frame_time_ != 0) {
        delta_seconds = std::clamp(
            static_cast<double>(frame_time - drag_last_frame_time_) / 1'000'000.0,
            1.0 / 240.0,
            1.0 / 30.0
        );
    }
    drag_last_frame_time_ = frame_time;

    const double acceleration =
        (drag_target_y_ - drag_offset_y_) * kDragSpringStiffness -
        drag_velocity_y_ * kDragSpringDamping;
    drag_velocity_y_ += acceleration * delta_seconds;
    drag_offset_y_ += drag_velocity_y_ * delta_seconds;
    apply_drag_offset();

    if (dragging_receipt_) {
        const int card_height = std::max(1, gtk_widget_get_height(card_));
        const double card_bottom =
            static_cast<double>(kReceiptTopMargin + card_height) +
            drag_offset_y_;
        const double arm_line = static_cast<double>(trash_margin_top_) +
            static_cast<double>(kTrashTargetSize) * 0.44;
        set_trash_armed(
            drag_raw_y_ >= kMinimumDismissPointerTravel &&
            card_bottom >= arm_line
        );
    }

    if (drag_dismiss_pending_) {
        const double distance = std::max(1.0, drag_target_y_ - drag_dismiss_start_y_);
        const double progress = std::clamp(
            (drag_offset_y_ - drag_dismiss_start_y_) / distance,
            0.0,
            1.0
        );
        gtk_widget_set_opacity(card_, 1.0 - progress * progress);
    }

    const bool settled =
        std::abs(drag_target_y_ - drag_offset_y_) < 0.35 &&
        std::abs(drag_velocity_y_) < 2.0;
    if (!settled) return true;

    drag_offset_y_ = drag_target_y_;
    apply_drag_offset();
    drag_velocity_y_ = 0.0;
    drag_last_frame_time_ = 0;

    if (drag_dismiss_pending_) {
        gtk_widget_set_opacity(card_, 0.0);
        gtk_revealer_set_reveal_child(GTK_REVEALER(trash_revealer_), FALSE);
        dismiss();
        return false;
    }

    // While the pointer is still held, settling only means that the receipt
    // caught up to its latest target. Do not spring it home or hide the trash.
    if (dragging_receipt_) return false;

    drag_offset_y_ = 0.0;
    drag_target_y_ = 0.0;
    apply_drag_offset();
    set_trash_armed(false);
    gtk_revealer_set_reveal_child(GTK_REVEALER(trash_revealer_), FALSE);
    schedule_auto_dismiss();
    return false;
}

void CommandReceiptOverlay::apply_drag_offset() {
    if (reveal_ == nullptr) return;
    gtk_widget_set_margin_top(
        reveal_,
        kReceiptTopMargin + static_cast<int>(std::lround(drag_offset_y_))
    );
}

void CommandReceiptOverlay::reset_drag_visuals() {
    if (drag_tick_id_ != 0 && reveal_ != nullptr) {
        gtk_widget_remove_tick_callback(reveal_, drag_tick_id_);
        drag_tick_id_ = 0;
    }
    dragging_receipt_ = false;
    drag_blocked_ = false;
    drag_dismiss_pending_ = false;
    drag_raw_y_ = 0.0;
    drag_offset_y_ = 0.0;
    drag_target_y_ = 0.0;
    drag_velocity_y_ = 0.0;
    drag_dismiss_start_y_ = 0.0;
    drag_pointer_start_y_ = 0.0;
    drag_pointer_has_surface_origin_ = false;
    drag_last_frame_time_ = 0;
    set_trash_armed(false);
    if (card_ != nullptr) {
        gtk_widget_remove_css_class(card_, "dragging");
        gtk_widget_remove_css_class(card_, "discarding");
        gtk_widget_set_opacity(card_, 1.0);
    }
    if (trash_revealer_ != nullptr) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(trash_revealer_), FALSE);
        gtk_widget_set_visible(trash_revealer_, FALSE);
    }
    apply_drag_offset();
}

gboolean CommandReceiptOverlay::drag_tick(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer user_data
) {
    auto* self = static_cast<CommandReceiptOverlay*>(user_data);
    if (self->advance_drag_frame(frame_clock)) return G_SOURCE_CONTINUE;
    self->drag_tick_id_ = 0;
    return G_SOURCE_REMOVE;
}

std::vector<std::string> CommandReceiptOverlay::execution_argv(
    const services::LauncherResult& result
) const {
    if (result.kind == services::LauncherResultKind::Command) {
        return {"fish", "-C", result.id};
    }
    if (result.kind == services::LauncherResultKind::Action && !result.id.empty()) {
        return {"/bin/bash", result.id};
    }
    return {};
}

std::string CommandReceiptOverlay::display_command_for(
    const services::LauncherResult& result
) const {
    if (result.kind == services::LauncherResultKind::Command) return result.id;
    if (result.kind == services::LauncherResultKind::Action) {
        return "/bin/bash " + shell_quote(result.id);
    }
    return {};
}

std::string CommandReceiptOverlay::compact_output_summary(
    bool successful,
    std::string_view standard_output,
    std::string_view standard_error,
    std::string_view launch_error
) const {
    if (!launch_error.empty()) return tail_excerpt(launch_error);
    if (!successful) {
        if (const std::string error = tail_excerpt(standard_error); !error.empty()) {
            return error;
        }
        if (const std::string output = tail_excerpt(standard_output); !output.empty()) {
            return output;
        }
        return "The command exited with an error and produced no output.";
    }

    if (const std::string output = tail_excerpt(standard_output); !output.empty()) {
        return output;
    }
    if (const std::string warning = tail_excerpt(standard_error); !warning.empty()) {
        return warning;
    }
    return "Completed successfully.";
}

void CommandReceiptOverlay::communicate_finished(
    GObject* source_object,
    GAsyncResult* result,
    gpointer user_data
) {
    std::unique_ptr<CompletionPayload> payload(
        static_cast<CompletionPayload*>(user_data)
    );
    GSubprocess* process = G_SUBPROCESS(source_object);
    gchar* standard_output = nullptr;
    gchar* standard_error = nullptr;
    GError* error = nullptr;
    const gboolean communicated = g_subprocess_communicate_utf8_finish(
        process,
        result,
        &standard_output,
        &standard_error,
        &error
    );

    bool successful = false;
    int exit_code = -1;
    std::string launch_error;
    if (communicated) {
        successful = g_subprocess_get_successful(process);
        if (g_subprocess_get_if_exited(process)) {
            exit_code = g_subprocess_get_exit_status(process);
        } else if (g_subprocess_get_if_signaled(process)) {
            exit_code = 128 + g_subprocess_get_term_sig(process);
        }
    } else {
        launch_error = error != nullptr && error->message != nullptr
            ? error->message
            : "Unable to collect command output";
    }

    const double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - payload->started_at
    ).count();
    std::string output = bounded_output(
        standard_output != nullptr ? standard_output : ""
    );
    std::string errors = bounded_output(
        standard_error != nullptr ? standard_error : ""
    );
    g_free(standard_output);
    g_free(standard_error);
    if (error != nullptr) g_error_free(error);

    CommandReceiptOverlay* owner = payload->state->owner.load();
    if (owner == nullptr ||
        payload->generation != payload->state->generation.load()) {
        return;
    }
    owner->complete_execution(
        payload->generation,
        successful,
        exit_code,
        duration,
        std::move(output),
        std::move(errors),
        std::move(launch_error)
    );
}

gboolean CommandReceiptOverlay::auto_dismiss_timeout(gpointer user_data) {
    auto* self = static_cast<CommandReceiptOverlay*>(user_data);
    self->auto_dismiss_id_ = 0;
    if (!self->pointer_inside_ && !self->logs_expanded_) self->dismiss();
    return G_SOURCE_REMOVE;
}

} // namespace realmheart::ui
