#include "ui/bar/VerticalBar.hpp"

#include "core/Command.hpp"
#include "services/HyprlandWorkspaces.hpp"
#include "services/Notifications.hpp"

#include "services/BatteryService.hpp"
#include "services/MediaService.hpp"
#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/VerticalBarModel.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace realmheart::ui::bar {
namespace {

struct WorkspaceSectionState {
    GtkWidget* section = nullptr;
    guint timer_id = 0;
    GCancellable* cancellable = nullptr;
    bool in_flight = false;
};

struct StatusProbeResult {
    std::vector<realmheart::services::ServiceStatus> services;
    realmheart::services::NotificationSnapshot notifications;
};

struct StatusSectionState {
    GtkWidget* section = nullptr;
    guint timer_id = 0;
    GCancellable* cancellable = nullptr;
    std::shared_ptr<realmheart::services::NotificationHistory> notification_history;
    std::function<void()> toggle_sidebar;
    bool in_flight = false;
};

struct ClockState {
    GtkLabel* label = nullptr;
    guint timer_id = 0;
};

using WorkspaceSectionHandle = std::shared_ptr<WorkspaceSectionState>;
using StatusSectionHandle = std::shared_ptr<StatusSectionState>;

void add_css_provider(std::string_view css) {
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, std::string(css).c_str());

    if (GdkDisplay* display = gdk_display_get_default(); display != nullptr) {
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
    }
    g_object_unref(provider);
}

void install_bar_css() {
    add_css_provider(
        ".realmheart-bar {"
        "  background: alpha(#11111b, 0.94);"
        "  border-right: 1px solid alpha(#cba6f7, 0.38);"
        "  padding: 10px 8px;"
        "}"
        ".realmheart-bar-brand {"
        "  color: #f5c2e7;"
        "  font-weight: 900;"
        "  font-size: 15px;"
        "  letter-spacing: 0.04em;"
        "}"
        ".realmheart-bar-clock {"
        "  color: #cdd6f4;"
        "  font-weight: 800;"
        "  font-size: 13px;"
        "}"
        ".realmheart-bar-pill {"
        "  background: alpha(#313244, 0.82);"
        "  border: 1px solid alpha(#cba6f7, 0.18);"
        "  border-radius: 999px;"
        "  color: #bac2de;"
        "  font-weight: 800;"
        "  min-width: 34px;"
        "  min-height: 28px;"
        "}"
        ".realmheart-bar-pill-active {"
        "  background: alpha(#cba6f7, 0.28);"
        "  border-color: alpha(#cba6f7, 0.68);"
        "  color: #f5e0dc;"
        "}"
        ".realmheart-bar-pill-occupied {"
        "  color: #cdd6f4;"
        "  border-color: alpha(#89b4fa, 0.42);"
        "}"
        ".realmheart-bar-status {"
        "  background: alpha(#1e1e2e, 0.64);"
        "  border: 1px solid alpha(#585b70, 0.40);"
        "  border-radius: 14px;"
        "  min-width: 42px;"
        "  min-height: 34px;"
        "}"
        ".realmheart-bar-status-enabled {"
        "  border-color: alpha(#a6e3a1, 0.52);"
        "}"
        ".realmheart-bar-status-disabled {"
        "  opacity: 0.52;"
        "}"
        ".realmheart-bar-badge {"
        "  background: #f38ba8;"
        "  border-radius: 999px;"
        "  color: #11111b;"
        "  font-size: 9px;"
        "  font-weight: 900;"
        "  min-width: 15px;"
        "  min-height: 15px;"
        "  padding: 1px;"
        "}"
        ".realmheart-bar-section {"
        "  margin-top: 8px;"
        "  margin-bottom: 8px;"
        "}"
        ".realmheart-bar-fallback-icon {"
        "  color: #cba6f7;"
        "  font-weight: 900;"
        "  font-size: 12px;"
        "}"
    );
}

GtkWidget* make_icon_or_text(std::string_view icon_name, std::string_view fallback_text, int pixel_size = 22) {
    if (const auto path = resolve_project_icon(icon_name)) {
        GtkWidget* image = gtk_image_new_from_file(path->string().c_str());
        gtk_image_set_pixel_size(GTK_IMAGE(image), pixel_size);
        gtk_widget_set_size_request(image, pixel_size, pixel_size);
        return image;
    }

    GtkWidget* label = gtk_label_new(std::string(fallback_text).c_str());
    gtk_widget_add_css_class(label, "realmheart-bar-fallback-icon");
    return label;
}

GtkWidget* make_status_widget(const BarStatusSlot& slot, const std::function<void()>& on_click) {
    GtkWidget* box = gtk_button_new();
    gtk_widget_add_css_class(box, "realmheart-bar-status");
    gtk_widget_add_css_class(box, slot.enabled ? "realmheart-bar-status-enabled" : "realmheart-bar-status-disabled");
    gtk_widget_set_tooltip_text(box, slot.tooltip.c_str());
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);

    GtkWidget* overlay = gtk_overlay_new();
    GtkWidget* image = make_icon_or_text(slot.icon_name, slot.fallback_text);
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), image);

    if (!slot.badge_text.empty()) {
        GtkWidget* badge = gtk_label_new(slot.badge_text.c_str());
        gtk_widget_add_css_class(badge, "realmheart-bar-badge");
        gtk_widget_set_halign(badge, GTK_ALIGN_END);
        gtk_widget_set_valign(badge, GTK_ALIGN_START);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), badge);
    }

    gtk_button_set_child(GTK_BUTTON(box), overlay);
    auto* callback = new std::function<void()>(on_click);
    g_signal_connect_data(box, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        (*static_cast<std::function<void()>*>(data))();
    }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, G_CONNECT_DEFAULT);
    return box;
}

GtkWidget* make_workspace_pill(const realmheart::services::WorkspaceState& workspace) {
    GtkWidget* label = gtk_button_new_with_label(std::to_string(workspace.id).c_str());
    gtk_widget_add_css_class(label, "realmheart-bar-pill");
    if (workspace.active) gtk_widget_add_css_class(label, "realmheart-bar-pill-active");
    if (workspace.windows > 0) gtk_widget_add_css_class(label, "realmheart-bar-pill-occupied");
    gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(label, 4);
    gtk_widget_set_margin_bottom(label, 4);

    std::string tooltip = "Workspace " + std::to_string(workspace.id);
    if (!workspace.name.empty() && workspace.name != std::to_string(workspace.id)) {
        tooltip += " (" + workspace.name + ")";
    }
    tooltip += workspace.active ? ": active" : ": inactive";
    tooltip += ", windows=" + std::to_string(workspace.windows);
    gtk_widget_set_tooltip_text(label, tooltip.c_str());
    g_object_set_data(G_OBJECT(label), "realmheart-workspace-id", GINT_TO_POINTER(workspace.id));
    g_signal_connect(label, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer) {
        const int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "realmheart-workspace-id"));
        realmheart::core::run_background({
            "hyprctl",
            "dispatch",
            "hl.dsp.focus({ workspace = " + std::to_string(id) + " })"
        });
    }), nullptr);
    return label;
}

void render_workspace_section(
    GtkWidget* workspace_section,
    const realmheart::services::WorkspaceSnapshot& snapshot
) {
    const auto tooltip = realmheart::services::HyprlandWorkspaces::describe(snapshot);
    gtk_widget_set_tooltip_text(workspace_section, tooltip.c_str());

    while (GtkWidget* child = gtk_widget_get_first_child(workspace_section)) {
        gtk_box_remove(GTK_BOX(workspace_section), child);
    }
    for (const auto& workspace : build_workspace_pills(snapshot)) {
        gtk_box_append(GTK_BOX(workspace_section), make_workspace_pill(workspace));
    }
}

void render_status_section(GtkWidget* status_section, const StatusProbeResult& result, const std::function<void()>& on_click) {
    while (GtkWidget* child = gtk_widget_get_first_child(status_section)) {
        gtk_box_remove(GTK_BOX(status_section), child);
    }
    for (const auto& slot : build_status_slots(result.services, result.notifications)) {
        gtk_box_append(GTK_BOX(status_section), make_status_widget(slot, on_click));
    }
}

realmheart::core::CommandOptions worker_command_options(GCancellable* cancellable) {
    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::milliseconds(1000);
    options.terminate_grace = std::chrono::milliseconds(100);
    options.max_output_bytes = 32 * 1024;
    options.cancelled = [cancellable] {
        return cancellable != nullptr && g_cancellable_is_cancelled(cancellable);
    };
    return options;
}

void read_workspace_task(GTask* task, gpointer, gpointer, GCancellable* cancellable) {
    if (g_task_return_error_if_cancelled(task)) return;
    auto snapshot = realmheart::services::HyprlandWorkspaces::read(worker_command_options(cancellable));
    if (g_task_return_error_if_cancelled(task)) return;
    g_task_return_pointer(
        task,
        new realmheart::services::WorkspaceSnapshot(std::move(snapshot)),
        [](gpointer data) { delete static_cast<realmheart::services::WorkspaceSnapshot*>(data); }
    );
}

void complete_workspace_task(GObject*, GAsyncResult* result, gpointer) {
    const auto* task_handle = static_cast<WorkspaceSectionHandle*>(g_task_get_task_data(G_TASK(result)));
    if (task_handle == nullptr || !*task_handle) return;
    const auto state = *task_handle;
    state->in_flight = false;

    GError* error = nullptr;
    std::unique_ptr<realmheart::services::WorkspaceSnapshot> snapshot(
        static_cast<realmheart::services::WorkspaceSnapshot*>(g_task_propagate_pointer(G_TASK(result), &error))
    );
    if (error != nullptr) g_error_free(error);
    if (state->section == nullptr || !snapshot) return;
    render_workspace_section(state->section, *snapshot);
}

void launch_workspace_task(const WorkspaceSectionHandle& state) {
    if (state->section == nullptr || state->cancellable == nullptr || state->in_flight) return;
    state->in_flight = true;
    GTask* task = g_task_new(
        nullptr,
        state->cancellable,
        complete_workspace_task,
        nullptr
    );
    g_task_set_task_data(
        task,
        new WorkspaceSectionHandle(state),
        [](gpointer data) { delete static_cast<WorkspaceSectionHandle*>(data); }
    );
    g_task_run_in_thread(task, read_workspace_task);
    g_object_unref(task);
}

gboolean refresh_workspace_section(gpointer user_data) {
    const auto& state = *static_cast<WorkspaceSectionHandle*>(user_data);
    if (!state || state->section == nullptr) return G_SOURCE_REMOVE;
    launch_workspace_task(state);
    return G_SOURCE_CONTINUE;
}

void read_status_task(GTask* task, gpointer, gpointer task_data, GCancellable* cancellable) {
    static_cast<void>(cancellable);
    if (g_task_return_error_if_cancelled(task)) return;
    const auto* task_handle = static_cast<StatusSectionHandle*>(task_data);
    if (task_handle == nullptr || !*task_handle) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "missing status task state");
        return;
    }
    const auto& state = *task_handle;

    auto result = std::make_unique<StatusProbeResult>();

    // Integrate Battery
    realmheart::services::BatteryService battery;
    if (auto batt = battery.read()) {
        result->services.emplace_back(realmheart::services::ServiceStatus{
            "Battery", std::to_string(batt->percentage) + "% (" + batt->status + ")", true
        });
    }

    // Integrate Media
    realmheart::services::MediaService media;
    if (auto m = media.get_current_media()) {
        result->services.emplace_back(realmheart::services::ServiceStatus{"Media", m->title + " - " + m->artist, true});
    }

    result->notifications = state->notification_history->snapshot();
    if (g_task_return_error_if_cancelled(task)) return;
    g_task_return_pointer(task, result.release(), [](gpointer data) { delete static_cast<StatusProbeResult*>(data); });
}

void complete_status_task(GObject*, GAsyncResult* result, gpointer) {
    const auto* task_handle = static_cast<StatusSectionHandle*>(g_task_get_task_data(G_TASK(result)));
    if (task_handle == nullptr || !*task_handle) return;
    const auto state = *task_handle;
    state->in_flight = false;

    GError* error = nullptr;
    std::unique_ptr<StatusProbeResult> probe(
        static_cast<StatusProbeResult*>(g_task_propagate_pointer(G_TASK(result), &error))
    );
    if (error != nullptr) g_error_free(error);
    if (state->section == nullptr || !probe) return;
    render_status_section(state->section, *probe, state->toggle_sidebar);
}

void launch_status_task(const StatusSectionHandle& state) {
    if (state->section == nullptr || state->cancellable == nullptr || state->in_flight) return;
    state->in_flight = true;
    GTask* task = g_task_new(
        nullptr,
        state->cancellable,
        complete_status_task,
        nullptr
    );
    g_task_set_task_data(
        task,
        new StatusSectionHandle(state),
        [](gpointer data) { delete static_cast<StatusSectionHandle*>(data); }
    );
    g_task_run_in_thread(task, read_status_task);
    g_object_unref(task);
}

gboolean refresh_status_section(gpointer user_data) {
    const auto& state = *static_cast<StatusSectionHandle*>(user_data);
    if (!state || state->section == nullptr) return G_SOURCE_REMOVE;
    launch_status_task(state);
    return G_SOURCE_CONTINUE;
}

void update_clock_label(GtkLabel* label) {
    std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%H\n%M", &local_time);
    gtk_label_set_text(label, buffer);
}

void schedule_clock_tick(ClockState* state);

gboolean refresh_clock(gpointer user_data) {
    auto* state = static_cast<ClockState*>(user_data);
    state->timer_id = 0;
    if (state->label == nullptr) return G_SOURCE_REMOVE;
    update_clock_label(state->label);
    schedule_clock_tick(state);
    return G_SOURCE_REMOVE;
}

void schedule_clock_tick(ClockState* state) {
    const auto now = std::chrono::system_clock::now();
    const auto next_minute = std::chrono::time_point_cast<std::chrono::minutes>(now) + std::chrono::minutes(1);
    const auto delay = std::clamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(next_minute - now).count() + 20,
        static_cast<std::int64_t>(1),
        static_cast<std::int64_t>(60000)
    );
    state->timer_id = g_timeout_add(static_cast<guint>(delay), refresh_clock, state);
}

gboolean handle_key_pressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer user_data) {
    if (keyval != GDK_KEY_Escape) return GDK_EVENT_PROPAGATE;
    g_application_quit(G_APPLICATION(user_data));
    return GDK_EVENT_STOP;
}

void attach_escape_controller(GtkWidget* window, GtkApplication* application) {
    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(handle_key_pressed), application);
    gtk_widget_add_controller(window, key_controller);
}

void own_workspace_state(GtkWidget* section, const WorkspaceSectionHandle& state) {
    auto* handle = new WorkspaceSectionHandle(state);
    g_object_set_data_full(
        G_OBJECT(section),
        "realmheart-workspace-section-state",
        handle,
        [](gpointer data) {
            auto* owned = static_cast<WorkspaceSectionHandle*>(data);
            if (owned != nullptr && *owned) {
                const auto& state = *owned;
                if (state->timer_id != 0) g_source_remove(state->timer_id);
                state->timer_id = 0;
                state->section = nullptr;
                if (state->cancellable != nullptr) {
                    g_cancellable_cancel(state->cancellable);
                    g_clear_object(&state->cancellable);
                }
            }
            delete owned;
        }
    );
    state->timer_id = g_timeout_add_seconds(1, refresh_workspace_section, handle);
}

void own_status_state(GtkWidget* section, const StatusSectionHandle& state) {
    auto* handle = new StatusSectionHandle(state);
    g_object_set_data_full(
        G_OBJECT(section),
        "realmheart-status-section-state",
        handle,
        [](gpointer data) {
            auto* owned = static_cast<StatusSectionHandle*>(data);
            if (owned != nullptr && *owned) {
                const auto& state = *owned;
                if (state->timer_id != 0) g_source_remove(state->timer_id);
                state->timer_id = 0;
                state->section = nullptr;
                if (state->cancellable != nullptr) {
                    g_cancellable_cancel(state->cancellable);
                    g_clear_object(&state->cancellable);
                }
            }
            delete owned;
        }
    );
    state->timer_id = g_timeout_add_seconds(5, refresh_status_section, handle);
}

} // namespace

GtkWindow* present_vertical_bar(GtkApplication* application, std::function<void()> toggle_sidebar) {
    install_bar_css();

    GtkWidget* window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window), "Realmheart bar");
    gtk_window_set_default_size(GTK_WINDOW(window), kVerticalBarWidth, 720);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    apply_layer_surface(GTK_WINDOW(window), make_bar_surface_spec(kVerticalBarWidth));

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "realmheart-bar");
    gtk_widget_set_size_request(root, kVerticalBarWidth, -1);

    GtkWidget* brand = gtk_label_new("RH");
    gtk_widget_add_css_class(brand, "realmheart-bar-brand");
    gtk_widget_set_tooltip_text(brand, "Realmheart vertical bar");
    gtk_box_append(GTK_BOX(root), brand);

    GtkWidget* top_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(top_section, "realmheart-bar-section");
    gtk_widget_set_vexpand(top_section, FALSE);
    GtkWidget* sidebar_button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(sidebar_button), make_icon_or_text("widgets.svg", "W"));
    auto* sidebar_callback = new std::function<void()>(toggle_sidebar);
    g_signal_connect_data(sidebar_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        (*static_cast<std::function<void()>*>(data))();
    }), sidebar_callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, G_CONNECT_DEFAULT);
    gtk_box_append(GTK_BOX(top_section), sidebar_button);
    gtk_box_append(GTK_BOX(root), top_section);

    GtkWidget* workspace_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(workspace_section, "realmheart-bar-section");
    gtk_widget_set_vexpand(workspace_section, TRUE);
    gtk_widget_set_valign(workspace_section, GTK_ALIGN_CENTER);
    auto workspace_state = std::make_shared<WorkspaceSectionState>();
    workspace_state->section = workspace_section;
    workspace_state->cancellable = g_cancellable_new();
    realmheart::services::WorkspaceSnapshot pending_workspaces;
    pending_workspaces.error = "workspace probe pending";
    render_workspace_section(workspace_section, pending_workspaces);
    own_workspace_state(workspace_section, workspace_state);
    launch_workspace_task(workspace_state);
    gtk_box_append(GTK_BOX(root), workspace_section);

    GtkWidget* clock = gtk_label_new(nullptr);
    gtk_widget_add_css_class(clock, "realmheart-bar-clock");
    gtk_widget_set_margin_top(clock, 8);
    gtk_widget_set_margin_bottom(clock, 8);
    auto* clock_state = new ClockState{GTK_LABEL(clock), 0};
    g_object_set_data_full(
        G_OBJECT(clock),
        "realmheart-clock-state",
        clock_state,
        [](gpointer data) {
            auto* state = static_cast<ClockState*>(data);
            if (state != nullptr && state->timer_id != 0) g_source_remove(state->timer_id);
            delete state;
        }
    );
    update_clock_label(clock_state->label);
    schedule_clock_tick(clock_state);
    gtk_box_append(GTK_BOX(root), clock);

    GtkWidget* status_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(status_section, "realmheart-bar-section");
    auto status_state = std::make_shared<StatusSectionState>();
    status_state->section = status_section;
    status_state->cancellable = g_cancellable_new();
    status_state->notification_history = std::make_shared<realmheart::services::NotificationHistory>();
    status_state->toggle_sidebar = toggle_sidebar;
    g_object_set_data_full(G_OBJECT(status_section), "realmheart-sidebar-toggle", new std::function<void()>(toggle_sidebar),
        +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
    render_status_section(status_section, {{}, status_state->notification_history->snapshot()}, toggle_sidebar);
    own_status_state(status_section, status_state);
    launch_status_task(status_state);
    gtk_box_append(GTK_BOX(root), status_section);

    attach_escape_controller(window, application);
    gtk_window_set_child(GTK_WINDOW(window), root);
    gtk_window_present(GTK_WINDOW(window));
    return GTK_WINDOW(window);
}

} // namespace realmheart::ui::bar
