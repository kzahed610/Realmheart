#include "ui/ShellApp.hpp"

#include "core/ShellControl.hpp"
#include "core/TaskExecutor.hpp"
#include "effects/core/EffectRegistry.hpp"
#include "effects/core/TransitionTimeline.hpp"
#include "services/Audio.hpp"
#include "services/AudioMonitor.hpp"
#include "services/BatteryService.hpp"
#include "services/Brightness.hpp"
#include "services/HyprlandSession.hpp"
#include "services/HyprlandWorkspaces.hpp"
#include "services/LauncherService.hpp"
#include "services/MediaService.hpp"
#include "services/NotesService.hpp"
#include "services/NotificationDaemon.hpp"
#include "services/NotificationServer.hpp"
#include "services/Notifications.hpp"
#include "services/SessionManager.hpp"
#include "services/ThemeService.hpp"
#include "services/UtilityManager.hpp"
#include "ui/ImageFileFilters.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/MonitorResolver.hpp"
#include "ui/NotesOverlay.hpp"
#include "ui/NotificationToast.hpp"
#include "ui/NowPlayingOverlay.hpp"
#include "ui/OSDOverlay.hpp"
#include "ui/ShellState.hpp"
#include "ui/ThemeStyles.hpp"
#include "ui/bar/VerticalBar.hpp"
#include "ui/launcher/CommandReceiptOverlay.hpp"
#include "ui/launcher/LauncherOverlay.hpp"
#include "ui/lockscreen/LockSurface.hpp"
#include "ui/powermenu/PowerMenuProcess.hpp"
#include "ui/sidebar/RightSidebar.hpp"
#include "ui/sidebar/SidebarFrame.hpp"
#include "ui/wallpaper/WallpaperBackend.hpp"
#include "ui/wallpaper/WallpaperController.hpp"
#include "ui/workspace/WorkspaceOverviewOverlay.hpp"
#include "mana_core/ManaCoresSelector.hpp"

#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace realmheart::ui {
namespace {

std::string current_executable_path() {
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

std::filesystem::path user_media_directory(GUserDirectory directory, const char* fallback_name) {
    if (const char* configured = g_get_user_special_dir(directory);
        configured != nullptr && *configured != '\0') {
        return configured;
    }
    if (const char* home = g_get_home_dir(); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / fallback_name;
    }
    return std::filesystem::temp_directory_path() / "realmheart" / fallback_name;
}

// Boot-idle delay before the workspace overview prewarm fires. Long enough
// to stay out of the startup critical path, short enough to be warm before
// a user realistically presses the overview keybind.
constexpr int kWorkspaceOverviewPrewarmDelayMs = 12000;
// Staggered 500ms after the overview prewarm so the two hidden-frame warm
// maps never contend for the frame clock on the same tick.
constexpr int kRightSidebarPrewarmDelayMs = 12500;
constexpr std::string_view kManaCoresWorkspaceName = "realmheart-mana-core";
// Empty named workspace the lock choreography slides windows off to.
constexpr std::string_view kLockWorkspaceName = "realmheart-lock";
// Hyprland submap entered while the lockscreen is up. It declares no binds,
// so every compositor keybind (SUPER+num workspace peeks included) goes
// dead while locked; the lockscreen's own layer-shell keyboard grab still
// receives keystrokes for the password entry.
constexpr std::string_view kLockSubmapName = "realmheart-locked";

template <typename... Args>
void sidebar_input_debug(Args&&... args) {
    std::ofstream log(
        "/tmp/realmheart-sidebar-input.log",
        std::ios::app
    );
    if (!log) return;

    log << g_get_monotonic_time() << " ";
    (log << ... << std::forward<Args>(args));
    log << '\n';
}

constexpr int kHotspotInputCommitFrames = 30;
const effects::EffectId kSidebarSurfaceEffect = effects::resolve_effect(
    effects::EffectId::FadeScale,
    effects::EffectTargetType::Sidebar
);

struct HotspotInputSetup {
    int frames_remaining = kHotspotInputCommitFrames;
};

struct BackdropInputSetup {
    int frames_remaining = kHotspotInputCommitFrames;
    int sidebar_top_margin = 0;
    int sidebar_height = 1;
    int sidebar_width = sidebar::kDefaultSidebarFrameLayout.surface_width();
    int sidebar_right_margin = sidebar::kDefaultSidebarFrameLayout.right_margin;
    int hotspot_hit_width = sidebar::kDefaultSidebarFrameLayout.hotspot_hit_width;
};

void draw_hotspot_commit_pixel(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer
) {
    if (width <= 0 || height <= 0) return;

    // A single almost-transparent pixel column prevents GTK/GSK from
    // collapsing this otherwise visually empty layer surface. At 1/255 alpha
    // it is effectively invisible, while still guaranteeing a buffer commit.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0 / 255.0);
    cairo_rectangle(cr, static_cast<double>(width - 1), 0.0, 1.0, height);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_backdrop_commit_fill(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer
) {
    if (width <= 0 || height <= 0) return;

    // Keep a real full-screen buffer alive at the minimum practical alpha.
    // This is visually indistinguishable from transparent, but prevents GTK
    // from collapsing the backdrop into an empty, click-through surface.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    // Keep one almost-transparent committed buffer so GTK/GSK does not
    // collapse the input-only backdrop surface.
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0 / 255.0);
    cairo_paint(cr);
    cairo_restore(cr);
}

bool apply_hotspot_input_region(GtkWidget* widget) {
    GtkNative* native = gtk_widget_get_native(widget);
    if (native == nullptr) return false;

    GdkSurface* surface = gtk_native_get_surface(native);
    if (surface == nullptr || !gdk_surface_get_mapped(surface)) return false;

    const int width = gdk_surface_get_width(surface);
    const int height = gdk_surface_get_height(surface);
    if (width <= 0 || height <= 0) return false;

    const cairo_rectangle_int_t rectangle{0, 0, width, height};
    cairo_region_t* region = cairo_region_create_rectangle(&rectangle);
    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);

    // Wayland input-region state is committed with the next surface commit.
    // Force a render while the setup callback is active so the region cannot
    // remain pending on an otherwise static transparent surface.
    gdk_surface_queue_render(surface);
    return true;
}

gboolean enforce_hotspot_input_region_on_tick(
    GtkWidget* widget,
    GdkFrameClock*,
    gpointer data
) {
    auto* setup = static_cast<HotspotInputSetup*>(data);

    if (!apply_hotspot_input_region(widget)) {
        return G_SOURCE_CONTINUE;
    }

    gtk_widget_queue_draw(widget);
    --setup->frames_remaining;
    return setup->frames_remaining > 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void destroy_hotspot_input_setup(gpointer data) {
    delete static_cast<HotspotInputSetup*>(data);
}

void enforce_hotspot_input_region(GtkWindow* window) {
    gtk_widget_add_tick_callback(
        GTK_WIDGET(window),
        enforce_hotspot_input_region_on_tick,
        new HotspotInputSetup{},
        destroy_hotspot_input_setup
    );
}

bool apply_sidebar_backdrop_input_region(
    GtkWidget* widget,
    const BackdropInputSetup& setup
) {
    GtkNative* native = gtk_widget_get_native(widget);
    if (native == nullptr) return false;

    GdkSurface* surface = gtk_native_get_surface(native);
    if (surface == nullptr || !gdk_surface_get_mapped(surface)) return false;

    const int width = gdk_surface_get_width(surface);
    const int height = gdk_surface_get_height(surface);
    if (width <= 0 || height <= 0) return false;

    const cairo_rectangle_int_t full_surface{0, 0, width, height};
    cairo_region_t* region = cairo_region_create_rectangle(&full_surface);

    // Keep the fullscreen Overlay surface reactive only outside the sidebar.
    // If the backdrop is stacked above the sidebar, the carved-out region lets
    // input fall through; if it is below, the result is identical.
    const int sidebar_x = std::max(
        width - setup.sidebar_right_margin - setup.sidebar_width,
        0
    );
    const int sidebar_y = std::clamp(setup.sidebar_top_margin, 0, height);
    const int sidebar_region_width = std::clamp(
        setup.sidebar_width,
        0,
        width - sidebar_x
    );
    const int sidebar_region_height = std::clamp(
        setup.sidebar_height,
        0,
        height - sidebar_y
    );

    if (sidebar_region_width > 0 && sidebar_region_height > 0) {
        const cairo_rectangle_int_t sidebar_rectangle{
            sidebar_x,
            sidebar_y,
            sidebar_region_width,
            sidebar_region_height
        };
        cairo_region_subtract_rectangle(region, &sidebar_rectangle);

        // The same invisible edge remains a close target while the sidebar is
        // open, even though most of the sidebar rectangle is click-through.
        const int edge_x = std::max(width - setup.hotspot_hit_width, 0);
        const cairo_rectangle_int_t edge_rectangle{
            edge_x,
            sidebar_y,
            width - edge_x,
            sidebar_region_height
        };
        cairo_region_union_rectangle(region, &edge_rectangle);
    }

    sidebar_input_debug(
        "backdrop region: surface=", width, "x", height,
        " sidebar=", sidebar_x, ",", sidebar_y, " ",
        sidebar_region_width, "x", sidebar_region_height,
        " edge_width=", setup.hotspot_hit_width
    );

    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
    gdk_surface_queue_render(surface);
    return true;
}

gboolean enforce_sidebar_backdrop_input_region_on_tick(
    GtkWidget* widget,
    GdkFrameClock*,
    gpointer data
) {
    auto* setup = static_cast<BackdropInputSetup*>(data);

    if (!apply_sidebar_backdrop_input_region(widget, *setup)) {
        return G_SOURCE_CONTINUE;
    }

    gtk_widget_queue_draw(widget);
    --setup->frames_remaining;
    return setup->frames_remaining > 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void destroy_backdrop_input_setup(gpointer data) {
    delete static_cast<BackdropInputSetup*>(data);
}

void enforce_sidebar_backdrop_input_region(
    GtkWindow* window,
    const sidebar::SidebarPlacement& placement
) {
    auto* setup = new BackdropInputSetup{};
    setup->sidebar_top_margin = placement.top_margin;
    setup->sidebar_height = placement.height;
    setup->sidebar_width = placement.frame_layout.surface_width();
    setup->sidebar_right_margin = placement.frame_layout.right_margin;
    setup->hotspot_hit_width = placement.frame_layout.hotspot_hit_width;

    gtk_widget_add_tick_callback(
        GTK_WIDGET(window),
        enforce_sidebar_backdrop_input_region_on_tick,
        setup,
        destroy_backdrop_input_setup
    );
}

} // namespace

class ShellRuntime {
private:
    struct RuntimeAsyncState {
        std::atomic<bool> alive{true};
        std::atomic<ShellRuntime*> owner{nullptr};
        std::atomic<std::uint64_t> volume_generation{0};
        std::atomic<std::uint64_t> brightness_generation{0};
        std::atomic<std::uint64_t> theme_generation{0};
    };

    struct NowPlayingAsyncState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> generation{0};
        std::atomic<bool> refresh_in_flight{false};
        std::atomic<bool> refresh_pending{false};
        std::atomic<ShellRuntime*> owner{nullptr};
    };

    struct MonitorHotspot {
        int monitor_index = 0;
        GtkWindow* window = nullptr;
        GtkWidget* button = nullptr;
        GtkWidget* commit_pixel = nullptr;
    };

    struct LockRestorePoint {
        std::string connector;
        int workspace_id = 0;
    };

public:
    ShellRuntime(
        GtkApplication* application,
        wallpaper::WallpaperBackendType wallpaper_backend
    )
        : application_(application),
          requested_wallpaper_backend_(wallpaper_backend),
          notification_server_(notification_history_),
          notification_daemon_(notification_server_, notification_history_),
          theme_service_(std::make_shared<services::ThemeService>()),
          utilities_(std::make_shared<services::UtilityManager>(theme_service_)),
          session_(std::make_unique<services::SessionManager>()),
          battery_(std::make_unique<services::BatteryService>()),
          media_(std::make_shared<services::MediaService>()) {
        runtime_async_state_->owner.store(this);
        now_playing_async_state_->owner.store(this);

        notification_server_.set_notification_handler([this](const auto& entry) {
            const int monitor_index = invocation_monitor_index();
            ensure_toast_overlay(monitor_index);
            toast_->show(entry, 4000);
        });

        if (!notification_daemon_.start()) {
            std::cerr << "Unable to start Realmheart notification daemon\n";
        }
    }

    ~ShellRuntime() {
        power_menu_process_.close();

        cancel_workspace_overview_prewarm();
        cancel_right_sidebar_prewarm();

        // Stop callbacks that capture this before tearing down UI/controllers.
        runtime_async_state_->alive.store(false);
        runtime_async_state_->owner.store(nullptr);
        if (lock_surface_ != nullptr) {
            lock_surface_->hide_immediately();
        }
        for (const auto& mirror : lock_mirror_surfaces_) {
            if (mirror != nullptr) mirror->hide_immediately();
        }
        ++lock_choreography_generation_;
        ++runtime_async_state_->volume_generation;
        ++runtime_async_state_->brightness_generation;
        ++runtime_async_state_->theme_generation;
        now_playing_async_state_->alive.store(false);
        now_playing_async_state_->owner.store(nullptr);
        now_playing_subscription_.reset();
        notification_server_.set_notification_handler({});
        notification_daemon_.stop();
        audio_monitor_.reset();

        wallpaper_controller_.reset();
        workspace_overview_.reset();
        launcher_overlay_.reset();
        command_receipts_.reset();
        lock_mirror_surfaces_.clear();
        lock_surface_.reset();
        notes_overlay_.reset();
        toast_.reset();
        osd_.reset();
        now_playing_.reset();

        if (sidebar_tick_id_ != 0 && sidebar_ != nullptr) {
            gtk_widget_remove_tick_callback(sidebar_->get_window(), sidebar_tick_id_);
            sidebar_tick_id_ = 0;
            sidebar_last_frame_time_ = 0;
        }
        sidebar_.reset();
        secondary_bars_.clear();
        bar_.reset();

        if (sidebar_backdrop_ != nullptr) {
            gtk_window_destroy(sidebar_backdrop_);
            sidebar_backdrop_ = nullptr;
        }
        destroy_monitor_hotspots();

        if (monitor_rebuild_idle_id_ != 0) {
            g_source_remove(monitor_rebuild_idle_id_);
            monitor_rebuild_idle_id_ = 0;
        }
        if (monitor_model_ != nullptr && monitor_model_signal_id_ != 0) {
            g_signal_handler_disconnect(monitor_model_, monitor_model_signal_id_);
            monitor_model_signal_id_ = 0;
        }
        monitor_model_ = nullptr;

        // Unsubscribe/remove the display-wide CSS provider while GTK is alive.
        theme_styles_.reset();
    }

    void activate() {
        ensure_core_initialized();
        state_.show_bar();
        apply_bar_visibility();
        schedule_workspace_overview_prewarm();
        schedule_right_sidebar_prewarm();
        const std::string current_path = utilities_->load_wallpaper_path();
        if (current_path.empty()) {
            // Per-output selections are independent state. They must still be
            // restored when the legacy/global wallpaper state is absent.
            restore_monitor_wallpapers();
            return;
        }
        request_wallpaper(
            current_path,
            "Unable to restore wallpaper",
            [this](bool, std::string) {
                // Restore output-specific overrides even if the global fallback
                // failed to decode; a valid monitor-local wallpaper should not
                // disappear because unrelated global state is stale.
                restore_monitor_wallpapers();
            }
        );
    }

    void toggle_character() {
        ensure_sidebar_initialized(invocation_monitor_index());
        sidebar_->toggle_character();
    }

    void set_character_hair_mode(std::string_view mode_name) {
        ensure_sidebar_initialized(invocation_monitor_index());
        static_cast<void>(sidebar_->set_character_hair_mode(mode_name));
    }

    void toggle_right_sidebar() {
        toggle_right_sidebar_on_monitor(invocation_monitor_index());
    }

    void toggle_right_sidebar_on_monitor(int monitor_index) {
        cancel_right_sidebar_prewarm();

        if (sidebar_ != nullptr && sidebar_monitor_index_ != monitor_index &&
            state_.right_sidebar_visible()) {
            gtk_widget_set_visible(sidebar_->get_window(), FALSE);
            if (sidebar_backdrop_ != nullptr) {
                gtk_widget_set_visible(GTK_WIDGET(sidebar_backdrop_), FALSE);
            }
            state_.set_right_sidebar_visible(false);
            sidebar_transition_.snap_hidden();
            sidebar_.reset();
            if (sidebar_backdrop_ != nullptr) {
                gtk_window_destroy(sidebar_backdrop_);
                sidebar_backdrop_ = nullptr;
            }
        }

        active_monitor_index_ = monitor_index;
        ensure_sidebar_initialized(monitor_index);
        sidebar_->cancel_prewarm();
        const bool before = state_.right_sidebar_visible();
        state_.toggle_right_sidebar();
        if (state_.right_sidebar_visible()) {
            sidebar_transition_.open();
        } else {
            sidebar_transition_.close();
        }
        sidebar_input_debug(
            "toggle_right_sidebar[monitor=", monitor_index, "]: ",
            before ? "open" : "closed",
            " -> ",
            state_.right_sidebar_visible() ? "open" : "closed"
        );
        apply_right_sidebar_visibility();
    }

    void show_osd_volume_value(double value, int monitor_index = -1) {
        if (monitor_index < 0) monitor_index = invocation_monitor_index();
        ensure_osd_overlay(monitor_index);
        osd_->show_volume(value);
    }

    void show_osd_brightness_value(double value, int monitor_index = -1) {
        if (monitor_index < 0) monitor_index = invocation_monitor_index();
        ensure_osd_overlay(monitor_index);
        osd_->show_brightness(value);
    }

    void show_osd_volume() {
        ensure_core_initialized();
        const int monitor_index = invocation_monitor_index();
        const auto state = runtime_async_state_;
        const std::uint64_t generation = state->volume_generation.fetch_add(1) + 1;
        static_cast<void>(core::shared_task_executor().post([state, generation, monitor_index] {
            std::optional<double> percent;
            if (const auto audio = services::Audio::read_default_sink()) {
                double volume = audio->volume;
                if (volume > 1.0) {
                    const auto normalized = services::Audio::set_default_sink_volume(1.0);
                    volume = normalized.success ? normalized.state.volume : 1.0;
                }
                percent = std::clamp(volume * 100.0, 0.0, 100.0);
            }

            struct Payload {
                std::shared_ptr<RuntimeAsyncState> state;
                std::uint64_t generation = 0;
                std::optional<double> percent;
                int monitor_index = 0;
            };
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    auto* owner = payload->state->owner.load();
                    if (payload->state->alive.load() && owner != nullptr &&
                        payload->state->volume_generation.load() == payload->generation &&
                        payload->percent) {
                        owner->show_osd_volume_value(
                            *payload->percent,
                            payload->monitor_index
                        );
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{state, generation, percent, monitor_index},
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        }));
    }

    void show_osd_brightness() {
        ensure_core_initialized();
        const int monitor_index = invocation_monitor_index();
        const auto state = runtime_async_state_;
        const std::uint64_t generation = state->brightness_generation.fetch_add(1) + 1;
        static_cast<void>(core::shared_task_executor().post([state, generation, monitor_index] {
            std::optional<double> percent;
            if (const auto brightness = services::Brightness::read()) {
                percent = brightness->percent;
            }

            struct Payload {
                std::shared_ptr<RuntimeAsyncState> state;
                std::uint64_t generation = 0;
                std::optional<double> percent;
                int monitor_index = 0;
            };
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    auto* owner = payload->state->owner.load();
                    if (payload->state->alive.load() && owner != nullptr &&
                        payload->state->brightness_generation.load() == payload->generation &&
                        payload->percent) {
                        owner->show_osd_brightness_value(
                            *payload->percent,
                            payload->monitor_index
                        );
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{state, generation, percent, monitor_index},
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        }));
    }

    void toggle_bar() {
        ensure_core_initialized();
        state_.toggle_bar();
        apply_bar_visibility();
    }

    void take_screenshot_full() {
        const auto path = user_media_directory(G_USER_DIRECTORY_PICTURES, "Pictures") /
            "Screenshots" /
            ("full_" + std::to_string(std::time(nullptr)) + ".png");
        utilities_->take_screenshot_full(path.string());
    }

    void take_screenshot_area() {
        utilities_->take_screenshot_area_to_clipboard();
    }

    void extract_ocr_area() {
        utilities_->extract_text_from_area();
    }

    void launch_launcher() {
        launch_launcher_on_monitor(invocation_monitor_index());
    }

    void launch_launcher_on_monitor(int monitor_index) {
        active_monitor_index_ = monitor_index;
        ensure_launcher_initialized(monitor_index);
        launcher_overlay_->toggle();
    }

    void launch_launcher_query(const std::string& query) {
        const int monitor_index = invocation_monitor_index();
        active_monitor_index_ = monitor_index;
        ensure_launcher_initialized(monitor_index);
        launcher_overlay_->show_with_query(query);
    }

    [[nodiscard]] const services::WorkspaceSnapshot& workspace_snapshot_for_monitor(
        int monitor_index
    ) const {
        if (monitor_index >= 0 &&
            static_cast<std::size_t>(monitor_index) < monitor_workspace_snapshots_.size() &&
            monitor_workspace_snapshots_[static_cast<std::size_t>(monitor_index)].available) {
            return monitor_workspace_snapshots_[static_cast<std::size_t>(monitor_index)];
        }
        return workspace_snapshot_;
    }

    void apply_workspace_snapshot(
        int monitor_index,
        services::WorkspaceSnapshot snapshot
    ) {
        if (monitor_index >= 0) {
            const std::size_t index = static_cast<std::size_t>(monitor_index);
            if (monitor_workspace_snapshots_.size() <= index) {
                monitor_workspace_snapshots_.resize(index + 1);
            }
            monitor_workspace_snapshots_[index] = snapshot;
        }
        workspace_snapshot_ = snapshot;
        if (workspace_overview_ && overview_monitor_index_ == monitor_index) {
            workspace_overview_->set_workspace_snapshot(snapshot);
        }
    }

    void activate_overview_workspace(int workspace_id) {
        if (workspace_id <= 0) return;
        const std::string monitor = monitor_connector_for_index(
            gdk_display_get_default(), overview_monitor_index_
        );
        static_cast<void>(realmheart::core::shared_task_executor().post(
            [workspace_id, monitor] {
                const bool activated = monitor.empty()
                    ? services::HyprlandWorkspaces::switch_to(workspace_id)
                    : services::HyprlandWorkspaces::switch_to_on_monitor(
                          workspace_id, monitor
                      );
                if (!activated) {
                    std::cerr
                        << "[WorkspaceOverview] unable to activate workspace "
                        << workspace_id << '\n';
                }
            }
        ));
    }

    void activate_overview_window(int workspace_id, std::string address) {
        if (workspace_id <= 0 || address.empty()) {
            activate_overview_workspace(workspace_id);
            return;
        }

        const std::string monitor = monitor_connector_for_index(
            gdk_display_get_default(), overview_monitor_index_
        );
        static_cast<void>(realmheart::core::shared_task_executor().post(
            [workspace_id, address = std::move(address), monitor] {
                const bool workspace_activated = monitor.empty()
                    ? services::HyprlandWorkspaces::switch_to(workspace_id)
                    : services::HyprlandWorkspaces::switch_to_on_monitor(
                          workspace_id, monitor
                      );
                if (services::HyprlandSession::focus_window(address)) return;

                std::cerr
                    << "[WorkspaceOverview] unable to focus window "
                    << address << " on workspace " << workspace_id;
                if (!workspace_activated) {
                    std::cerr << " (workspace activation also failed)";
                }
                std::cerr << '\n';
            }
        ));
    }

    void move_overview_window(int workspace_id, std::string address) {
        if (workspace_id <= 0 || address.empty()) return;

        static_cast<void>(realmheart::core::shared_task_executor().post(
            [workspace_id, address = std::move(address)] {
                if (services::HyprlandSession::move_window_to_workspace(
                        address,
                        workspace_id
                    )) {
                    return;
                }
                std::cerr
                    << "[WorkspaceOverview] unable to move window "
                    << address << " to workspace " << workspace_id << '\n';
            }
        ));
    }

    void ensure_workspace_overview(int monitor_index) {
        if (workspace_overview_ != nullptr && overview_monitor_index_ != monitor_index) {
            workspace_overview_->hide();
            workspace_overview_.reset();
        }
        if (workspace_overview_) return;
        overview_monitor_index_ = monitor_index;
        workspace_overview_ =
            std::make_unique<workspace::WorkspaceOverviewOverlay>(
                application_,
                [this](int workspace_id) {
                    activate_overview_workspace(workspace_id);
                },
                [this](int workspace_id, std::string address) {
                    activate_overview_window(
                        workspace_id,
                        std::move(address)
                    );
                },
                [this](int workspace_id, std::string address) {
                    move_overview_window(
                        workspace_id,
                        std::move(address)
                    );
                },
                [this](bool active) {
                    if (auto* bar = bar_for_monitor(overview_monitor_index_)) {
                        bar->set_workspace_morph_active(active);
                    }
                },
                [this](double progress) {
                    if (auto* bar = bar_for_monitor(overview_monitor_index_)) {
                        bar->set_workspace_morph_progress(progress);
                    }
                },
                monitor_index
            );
    }

    // One-time boot warmup: the overview's first open costs ~600 ms of asset
    // decode + overlay rasterization + first-surface GL pipeline warmup. Pay
    // that at boot-idle instead of on the user's first keybind press. The
    // hidden-frame warm map keeps the surface mapped-but-invisible; measured
    // idle cost is ~0.7 MB RSS and zero GPU work when not animating.
    void prewarm_workspace_overview() {
        ensure_workspace_overview(0);
        workspace_overview_->set_workspace_snapshot(
            workspace_snapshot_for_monitor(0)
        );
        workspace_overview_->prewarm();
    }

    void schedule_workspace_overview_prewarm() {
        if (workspace_overview_prewarm_id_ != 0) return;
        workspace_overview_prewarm_id_ = g_timeout_add_full(
            G_PRIORITY_LOW,
            kWorkspaceOverviewPrewarmDelayMs,
            +[](gpointer raw) -> gint {
                auto* runtime = static_cast<ShellRuntime*>(raw);
                runtime->workspace_overview_prewarm_id_ = 0;
                runtime->prewarm_workspace_overview();
                return G_SOURCE_REMOVE;
            },
            this,
            nullptr
        );
    }

    void cancel_workspace_overview_prewarm() {
        if (workspace_overview_prewarm_id_ != 0) {
            g_source_remove(workspace_overview_prewarm_id_);
            workspace_overview_prewarm_id_ = 0;
        }
    }

    // One-time boot warmup for the right sidebar: constructing the panel and
    // its fullscreen backdrop costs ~450ms on the first toggle. Build both at
    // boot-idle (mapping nothing), then map the panel once at opacity 0 for a
    // single frame tick so its first real open is as cheap as later ones.
    // The backdrop is deliberately NOT mapped - its first map is cheap and a
    // fullscreen input surface has no business appearing uninvited.
    void prewarm_right_sidebar() {
        ensure_sidebar_initialized(0);
        sidebar_->prewarm();
    }

    void schedule_right_sidebar_prewarm() {
        if (right_sidebar_prewarm_id_ != 0) return;
        right_sidebar_prewarm_id_ = g_timeout_add_full(
            G_PRIORITY_LOW,
            kRightSidebarPrewarmDelayMs,
            +[](gpointer raw) -> gint {
                auto* runtime = static_cast<ShellRuntime*>(raw);
                runtime->right_sidebar_prewarm_id_ = 0;
                runtime->prewarm_right_sidebar();
                return G_SOURCE_REMOVE;
            },
            this,
            nullptr
        );
    }

    void cancel_right_sidebar_prewarm() {
        if (right_sidebar_prewarm_id_ != 0) {
            g_source_remove(right_sidebar_prewarm_id_);
            right_sidebar_prewarm_id_ = 0;
        }
    }

    void toggle_workspace_overview() {
        toggle_workspace_overview_on_monitor(invocation_monitor_index());
    }

    void toggle_workspace_overview_on_monitor(int monitor_index) {
        cancel_workspace_overview_prewarm();
        ensure_core_initialized();
        active_monitor_index_ = monitor_index;
        ensure_workspace_overview(monitor_index);
        // Refresh the snapshot on every toggle so a boot-time prewarm never
        // serves stale workspace data at open time.
        workspace_overview_->set_workspace_snapshot(
            workspace_snapshot_for_monitor(monitor_index)
        );
        if (auto* bar = bar_for_monitor(monitor_index)) {
            workspace_overview_->set_morph_sources(bar->workspace_morph_sources());
        } else {
            workspace_overview_->set_morph_sources({});
        }
        workspace_overview_->toggle();
    }

    void toggle_mana_cores() {
        const int monitor_index = invocation_monitor_index();
        active_monitor_index_ = monitor_index;
        auto* invocation_bar = bar_for_monitor(monitor_index);
        if (invocation_bar == nullptr ||
            !gtk_widget_get_realized(invocation_bar->get_window())) {
            if (mana_cores_deferred_toggle_count_++ >= 10) {
                mana_cores_deferred_toggle_count_ = 0;
                std::cerr << "[ManaCores] bar not realized after retries, aborting toggle\n";
                return;
            }
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* runtime = static_cast<ShellRuntime*>(raw);
                    runtime->toggle_mana_cores();
                    return G_SOURCE_REMOVE;
                },
                this,
                nullptr
            );
            return;
        }

        mana_cores_deferred_toggle_count_ = 0;
        ensure_core_initialized();

        if (mana_cores_launch_pending_ ||
            mana_cores_restore_pending_) {
            return;
        }

        // Check if ManaCores selector is already active
        if (mana_cores_selector_ && mana_cores_selector_->is_visible()) {
            mana_cores_selector_->request_dismiss();
            // request_dismiss() triggers the animated dismiss sequence
            // (Contraction -> Slide -> dismiss_callback restores workspace/bar).
            return;
        }

        const std::string monitor_connector = monitor_connector_for_index(
            gdk_display_get_default(), monitor_index
        );
        // No active wallpaper is fine: load_wallpapers_from_library() discovers
        // ~/Pictures/Wallpapers on its own and picks index 0 when the path is empty.
        // Prefer this output's persisted override so the selector previews the
        // wallpaper that is actually visible on the monitor where it opened.
        std::string current_path = utilities_->load_wallpaper_path();
        if (!monitor_connector.empty()) {
            if (services::WallpaperService* service =
                    utilities_->get_wallpaper_service()) {
                if (const auto output_path =
                        service->load_output_path(monitor_connector)) {
                    current_path = output_path->string();
                }
            }
        }

        // Dismiss transient shell surfaces and hide the bar
        power_menu_process_.close();
        if (launcher_overlay_) launcher_overlay_->hide();
        if (workspace_overview_ && workspace_overview_->visible()) {
            workspace_overview_->hide();
        }
        if (state_.right_sidebar_visible()) {
            toggle_right_sidebar_on_monitor(
                sidebar_monitor_index_ >= 0 ? sidebar_monitor_index_ : monitor_index
            );
        }

        mana_cores_bar_was_visible_ = bar_ != nullptr && state_.bar_visible();
        if (mana_cores_bar_was_visible_) hide_all_bars();

        mana_cores_launch_pending_ = true;
        const std::uint64_t generation = ++mana_cores_launch_generation_;
        const auto& monitor_workspace = workspace_snapshot_for_monitor(monitor_index);
        const int cached_workspace = monitor_workspace.available
            ? monitor_workspace.active_id
            : 0;
        const auto async_state = runtime_async_state_;

        const bool posted = core::shared_task_executor().post([
            async_state,
            generation,
            cached_workspace,
            current_path,
            monitor_index,
            monitor_connector
        ] {
            int original_workspace = cached_workspace;
            if (!monitor_connector.empty()) {
                const auto snapshot =
                    services::HyprlandWorkspaces::read_for_monitor(monitor_connector);
                if (snapshot.available) original_workspace = snapshot.active_id;
            } else if (const auto active =
                           services::HyprlandWorkspaces::active_workspace_id()) {
                original_workspace = *active;
            }

            const bool switched = original_workspace != 0 &&
                (monitor_connector.empty()
                    ? services::HyprlandWorkspaces::switch_to_named(
                          kManaCoresWorkspaceName
                      )
                    : services::HyprlandWorkspaces::switch_to_named_on_monitor(
                          kManaCoresWorkspaceName, monitor_connector
                      ));

            struct Payload {
                std::shared_ptr<RuntimeAsyncState> state;
                std::uint64_t generation = 0;
                std::string current_path;
                int original_workspace = 0;
                bool switched = false;
                int monitor_index = 0;
                std::string monitor_connector;
            };

            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    ShellRuntime* owner = payload->state->owner.load();
                    if (payload->state->alive.load() && owner != nullptr) {
                        owner->finish_mana_cores_launch(
                            payload->generation,
                            std::move(payload->current_path),
                            payload->original_workspace,
                            payload->switched,
                            payload->monitor_index,
                            std::move(payload->monitor_connector)
                        );
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{
                    async_state,
                    generation,
                    current_path,
                    original_workspace,
                    switched,
                    monitor_index,
                    monitor_connector,
                },
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        });

        if (!posted) {
            mana_cores_launch_pending_ = false;
            restore_mana_cores_chrome();
            std::cerr << "[ManaCores] unable to queue workspace handoff\n";
        }
    }

    void set_wallpaper(const std::string& path = {}) {
        ensure_core_initialized();
        if (mana_cores_launch_pending_ || mana_cores_restore_pending_) {
            std::cerr
                << "[ManaCores] wallpaper changes are locked during an active transaction\n";
            return;
        }
        if (path.empty()) {
            choose_wallpaper_native();
            return;
        }

        request_wallpaper(path, "Unable to set wallpaper");
    }

    void switch_wallpaper_backend(const std::string& backend_name) {
        ensure_core_initialized();
        if (mana_cores_launch_pending_ || mana_cores_restore_pending_) {
            std::cerr
                << "[ManaCores] wallpaper backend changes are locked during an active transaction\n";
            return;
        }
        const auto backend = wallpaper::parse_wallpaper_backend_type(backend_name);
        if (!backend) {
            std::cerr << "Unknown wallpaper backend: " << backend_name
                      << " (expected gtk or native)\n";
            return;
        }

        const auto selected_backend = *backend;
        wallpaper_controller_->switch_backend_async(
            selected_backend,
            [this, selected_backend, backend_name](
                bool success,
                std::string error_message
            ) {
                if (!success) {
                    std::cerr << "Unable to switch wallpaper backend to "
                              << backend_name << ": " << error_message << '\n';
                    return;
                }
                requested_wallpaper_backend_ = selected_backend;
                std::cerr << "Wallpaper backend switched to " << backend_name << '\n';
            }
        );
    }

    void inject_stress_notifications(std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            notification_server_.notify(
                "realmheart-lifetime-stress",
                0,
                "Lifetime notification " + std::to_string(index),
                "Exercises bounded active IDs and coalesced GTK refreshes"
            );
        }
    }

    void choose_wallpaper_native() {
        GtkFileDialog* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Choose Wallpaper");

        GListModel* filters = create_image_file_filters();
        gtk_file_dialog_set_filters(dialog, filters);
        g_object_unref(filters);

        gtk_file_dialog_open(
            dialog,
            nullptr,
            nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                auto* runtime = static_cast<ShellRuntime*>(data);
                GError* error = nullptr;
                GFile* file = gtk_file_dialog_open_finish(
                    GTK_FILE_DIALOG(source),
                    result,
                    &error
                );
                if (file != nullptr) {
                    char* path = g_file_get_path(file);
                    if (path != nullptr) {
                        runtime->set_wallpaper(path);
                        g_free(path);
                    }
                    g_object_unref(file);
                } else if (error != nullptr) {
                    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
                        std::cerr << "Wallpaper picker failed: " << error->message << '\n';
                    }
                    g_error_free(error);
                }
            },
            this
        );
        g_object_unref(dialog);
    }

    void generate_theme() {
        generate_theme_for(utilities_->load_wallpaper_path());
    }

    void start_recording() {
        const auto path = user_media_directory(G_USER_DIRECTORY_VIDEOS, "Videos") /
            "Recordings" /
            ("rec_" + std::to_string(std::time(nullptr)) + ".mp4");
        utilities_->start_recording(path.string());
    }

    void stop_recording() {
        utilities_->stop_recording();
    }

    void toggle_notes() {
        const int monitor_index = invocation_monitor_index();
        active_monitor_index_ = monitor_index;
        ensure_notes_overlay(monitor_index);
        notes_overlay_->toggle();
    }

    void ensure_lock_surfaces(int primary_monitor_index) {
        GdkDisplay* display = gdk_display_get_default();
        const int count = std::max(monitor_count(display), 1);
        primary_monitor_index = std::clamp(primary_monitor_index, 0, count - 1);

        const bool topology_matches = lock_surface_ != nullptr &&
            lock_surface_->monitor_index() == primary_monitor_index &&
            static_cast<int>(lock_mirror_surfaces_.size()) == count - 1;
        if (topology_matches) return;

        if (lock_surface_ != nullptr) lock_surface_->hide_immediately();
        for (const auto& mirror : lock_mirror_surfaces_) {
            if (mirror != nullptr) mirror->hide_immediately();
        }
        lock_mirror_surfaces_.clear();
        lock_surface_.reset();

        lock_surface_ = std::make_unique<lockscreen::LockSurface>(
            application_, primary_monitor_index, true
        );
        lock_surface_->set_unlock_started_callback([this] {
            for (const auto& mirror : lock_mirror_surfaces_) {
                if (mirror != nullptr) mirror->hide();
            }
        });
        lock_surface_->set_unlocked_callback([this] {
            finish_lock_unlock();
        });

        lock_mirror_surfaces_.reserve(static_cast<std::size_t>(count - 1));
        for (int index = 0; index < count; ++index) {
            if (index == primary_monitor_index) continue;
            lock_mirror_surfaces_.push_back(
                std::make_unique<lockscreen::LockSurface>(
                    application_, index, false
                )
            );
        }
        lock_monitor_index_ = primary_monitor_index;
    }

    [[nodiscard]] bool all_native_lock_surfaces_visible() const {
        if (lock_surface_ == nullptr || !lock_surface_->mapped()) return false;
        for (const auto& mirror : lock_mirror_surfaces_) {
            if (mirror == nullptr || !mirror->mapped()) return false;
        }
        return true;
    }

    void hide_native_lock_surfaces_immediately() {
        if (lock_surface_ != nullptr) lock_surface_->hide_immediately();
        for (const auto& mirror : lock_mirror_surfaces_) {
            if (mirror != nullptr) mirror->hide_immediately();
        }
    }

    void fallback_to_hyprlock(std::string_view reason) {
        if (lock_hyprlock_fallback_active_) return;
        lock_hyprlock_fallback_active_ = true;
        lock_choreography_pending_ = false;
        ++lock_choreography_generation_;
        std::cerr << "[Lockscreen] " << reason
                  << "; falling back to hyprlock\n";
        hide_native_lock_surfaces_immediately();
        if (!session_->lock()) {
            std::cerr << "[Lockscreen] unable to launch hyprlock fallback\n";
            lock_hyprlock_fallback_active_ = false;
            finish_lock_unlock();
            return;
        }

        struct HyprlockWatch {
            std::shared_ptr<RuntimeAsyncState> state;
        };
        g_timeout_add_full(
            G_PRIORITY_DEFAULT,
            500,
            +[](gpointer raw) -> gboolean {
                auto* watch = static_cast<HyprlockWatch*>(raw);
                ShellRuntime* owner = watch->state->owner.load();
                if (!watch->state->alive.load() || owner == nullptr) {
                    return G_SOURCE_REMOVE;
                }
                const auto hyprlock = realmheart::core::run_capture(
                    {"pgrep", "-x", "hyprlock"}
                );
                if (!hyprlock.succeeded() || hyprlock.output.find_first_not_of(
                        " \t\n\r0123456789") != std::string::npos) {
                    owner->finish_lock_unlock();
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            },
            new HyprlockWatch{runtime_async_state_},
            +[](gpointer raw) { delete static_cast<HyprlockWatch*>(raw); }
        );
    }

    void lock_session() {
        // Ignore re-entry while already locked (SUPER+L while locked must not
        // toggle back to the desktop).
        if (lock_hyprlock_fallback_active_ || lock_choreography_pending_ ||
            (lock_surface_ != nullptr && lock_surface_->visible())) {
            return;
        }

        const int primary_monitor_index = invocation_monitor_index();
        ensure_lock_surfaces(primary_monitor_index);

        // Lock choreography (mana-core style): every output moves to its own
        // empty named workspace, then a Broken Seal surface is mapped on every
        // monitor. Only the invoking monitor owns password input; the others
        // are visual mirrors, so the custom lockscreen stays Realmheart-native
        // without competing exclusive keyboard grabs.
        lock_choreography_pending_ = true;
        hide_native_lock_surfaces_immediately();

        // Jail compositor binds BEFORE anything else: from this instant,
        // SUPER+num and friends cannot move focus to a workspace with real
        // windows. The interactive layer-shell surface receives password input.
        services::HyprlandWorkspaces::set_submap(kLockSubmapName);
        lock_choreography_bar_was_visible_ =
            bar_ != nullptr && state_.bar_visible();
        if (lock_choreography_bar_was_visible_) hide_all_bars();

        GdkDisplay* display = gdk_display_get_default();
        const int count = std::max(monitor_count(display), 1);
        std::vector<std::string> connectors;
        connectors.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            connectors.push_back(monitor_connector_for_index(display, index));
        }

        const std::uint64_t generation = ++lock_choreography_generation_;
        const auto async_state = runtime_async_state_;
        const bool posted = core::shared_task_executor().post([
            async_state,
            generation,
            connectors = std::move(connectors)
        ]() mutable {
            bool switched = true;
            std::vector<LockRestorePoint> restore_points;
            restore_points.reserve(connectors.size());

            for (std::size_t index = 0; index < connectors.size(); ++index) {
                const std::string& connector = connectors[index];
                int original_workspace = 0;

                if (!connector.empty()) {
                    const auto snapshot =
                        services::HyprlandWorkspaces::read_for_monitor(connector);
                    if (snapshot.available) original_workspace = snapshot.active_id;
                } else if (connectors.size() == 1) {
                    original_workspace =
                        services::HyprlandWorkspaces::active_workspace_id().value_or(0);
                }

                restore_points.push_back({connector, original_workspace});
                if (original_workspace <= 0) {
                    switched = false;
                    continue;
                }

                const std::string workspace_name = connectors.size() == 1
                    ? std::string{kLockWorkspaceName}
                    : std::string{kLockWorkspaceName} + "-" +
                        std::to_string(index);
                const bool ok = connector.empty()
                    ? services::HyprlandWorkspaces::switch_to_named(workspace_name)
                    : services::HyprlandWorkspaces::switch_to_named_on_monitor(
                          workspace_name, connector
                      );
                switched = switched && ok;
            }

            struct Payload {
                std::shared_ptr<RuntimeAsyncState> state;
                std::uint64_t generation = 0;
                bool switched = false;
                std::vector<LockRestorePoint> restore_points;
            };
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    ShellRuntime* owner = payload->state->owner.load();
                    if (payload->state->alive.load() && owner != nullptr) {
                        owner->finish_lock_choreography(
                            payload->generation,
                            payload->switched,
                            std::move(payload->restore_points)
                        );
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{
                    async_state,
                    generation,
                    switched,
                    std::move(restore_points)
                },
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        });

        if (!posted) {
            lock_choreography_pending_ = false;
            fallback_to_hyprlock("unable to queue multi-monitor lock choreography");
        }
    }

    void finish_lock_choreography(
        std::uint64_t generation,
        bool switched,
        std::vector<LockRestorePoint> restore_points
    ) {
        if (generation != lock_choreography_generation_) return;
        lock_choreography_pending_ = false;
        lock_choreography_switched_ = switched;
        lock_restore_points_ = std::move(restore_points);

        if (lock_topology_dirty_) {
            fallback_to_hyprlock(
                "monitor topology changed during Broken Seal lock choreography"
            );
            return;
        }

        if (!switched) {
            fallback_to_hyprlock("unable to isolate every monitor workspace");
            return;
        }

        // No compositor blur by design: every output now shows an empty lock
        // workspace over its wallpaper. Map mirrors first, then the interactive
        // surface last so keyboard focus deterministically lands there.
        for (const auto& mirror : lock_mirror_surfaces_) {
            if (mirror != nullptr) mirror->show();
        }
        if (lock_surface_ != nullptr) lock_surface_->show();

        const auto async_state = runtime_async_state_;
        const std::uint64_t fallback_generation = generation;
        g_timeout_add(2000, +[](gpointer raw) -> gboolean {
            auto* payload = static_cast<std::pair<
                std::shared_ptr<RuntimeAsyncState>, std::uint64_t>*>(raw);
            ShellRuntime* owner = payload->first->owner.load();
            if (!payload->first->alive.load() || owner == nullptr ||
                owner->lock_choreography_generation_ != payload->second) {
                delete payload;
                return G_SOURCE_REMOVE;
            }
            if (owner->all_native_lock_surfaces_visible()) {
                delete payload;
                return G_SOURCE_REMOVE;
            }
            owner->fallback_to_hyprlock(
                "one or more Broken Seal monitor surfaces failed to map"
            );
            delete payload;
            return G_SOURCE_REMOVE;
        }, new std::pair<std::shared_ptr<RuntimeAsyncState>, std::uint64_t>{
            async_state, fallback_generation
        });
    }

    void finish_lock_unlock() {
        // Reverse choreography: mirrors disappear first, binds return, then
        // each monitor goes back to the workspace it owned before locking.
        for (const auto& mirror : lock_mirror_surfaces_) {
            if (mirror != nullptr) mirror->hide_immediately();
        }
        services::HyprlandWorkspaces::set_submap("reset");

        // Restore every monitor for which we captured an original workspace.
        // This is intentionally independent of the aggregate `switched` flag:
        // if one output failed to enter its lock workspace after another one
        // succeeded, the successful output still has to be restored.
        for (const auto& restore : lock_restore_points_) {
            if (restore.workspace_id <= 0) continue;
            if (!restore.connector.empty()) {
                services::HyprlandWorkspaces::switch_to_on_monitor(
                    restore.workspace_id, restore.connector
                );
            } else {
                services::HyprlandWorkspaces::switch_to(restore.workspace_id);
            }
        }
        lock_restore_points_.clear();

        if (lock_choreography_bar_was_visible_ && state_.bar_visible()) {
            present_all_bars();
        }
        lock_choreography_pending_ = false;
        lock_choreography_switched_ = false;
        lock_choreography_bar_was_visible_ = false;
        lock_hyprlock_fallback_active_ = false;

        // If the monitor model changed while Broken Seal was active, its old
        // wl_output-bound surfaces are intentionally kept alive until the
        // fallback/unlock completes. Drop them now so the next lock rebuilds
        // against the new topology instead of reusing stale output bindings.
        if (lock_topology_dirty_) {
            hide_native_lock_surfaces_immediately();
            lock_mirror_surfaces_.clear();
            lock_surface_.reset();
            lock_monitor_index_ = -1;
            lock_topology_dirty_ = false;
        }
    }

    void open_logout_menu(
        double origin_x = 24.0 / 1920.0,
        double origin_y = 1048.0 / 1080.0
    ) {
        power_menu_process_.toggle(invocation_monitor_index(), origin_x, origin_y);
    }

    void open_logout_menu_on_monitor(
        int monitor_index,
        double origin_x,
        double origin_y
    ) {
        active_monitor_index_ = monitor_index;
        power_menu_process_.toggle(monitor_index, origin_x, origin_y);
    }

    void quit() {
        g_application_quit(G_APPLICATION(application_));
    }

    void restart() {
        const auto backend = wallpaper_controller_ != nullptr
            ? wallpaper_controller_->active_backend()
            : requested_wallpaper_backend_;
        const std::string executable = current_executable_path();
        if (executable.empty()) {
            std::cerr << "Unable to restart Realmheart: /proc/self/exe could not be resolved: "
                      << std::strerror(errno) << '\n';
            return;
        }

        const std::string old_pid = std::to_string(::getpid());
        const std::string backend_name(
            wallpaper::wallpaper_backend_type_name(backend)
        );

        const pid_t helper = ::fork();
        if (helper < 0) {
            std::cerr << "Unable to restart Realmheart: fork failed: "
                      << std::strerror(errno) << '\n';
            return;
        }
        if (helper == 0) {
            static_cast<void>(::setsid());
            ::execl(
                executable.c_str(),
                executable.c_str(),
                "--restart-helper",
                old_pid.c_str(),
                backend_name.c_str(),
                static_cast<char*>(nullptr)
            );
            _exit(127);
        }

        g_application_quit(G_APPLICATION(application_));
    }

private:
    void request_now_playing_refresh() {
        const auto state = now_playing_async_state_;
        if (state->refresh_in_flight.exchange(true)) {
            state->refresh_pending = true;
            return;
        }

        const std::uint64_t generation = state->generation.fetch_add(1) + 1;
        const auto service = media_;
        const bool queued = core::shared_task_executor().post(
            [state, service, generation] {
                if (!state->alive.load() || !service) {
                    state->refresh_in_flight = false;
                    return;
                }
                auto info = service->get_current_media();

                struct Payload {
                    std::shared_ptr<NowPlayingAsyncState> state;
                    std::uint64_t generation = 0;
                    std::optional<services::MediaInfo> info;
                };

                g_idle_add_full(
                    G_PRIORITY_DEFAULT_IDLE,
                    +[](gpointer raw) -> gboolean {
                        auto* payload = static_cast<Payload*>(raw);
                        auto& state = *payload->state;
                        ShellRuntime* owner = state.owner.load();
                        if (state.alive.load() &&
                            state.generation.load() == payload->generation &&
                            owner != nullptr) {
                            owner->apply_now_playing_media(std::move(payload->info));
                        }

                        state.refresh_in_flight = false;
                        if (state.alive.load() && owner != nullptr &&
                            state.refresh_pending.exchange(false)) {
                            owner->request_now_playing_refresh();
                        }
                        return G_SOURCE_REMOVE;
                    },
                    new Payload{state, generation, std::move(info)},
                    +[](gpointer raw) { delete static_cast<Payload*>(raw); }
                );
            }
        );
        if (!queued) state->refresh_in_flight = false;
    }

    void start_now_playing_monitor() {
        if (now_playing_monitor_started_) return;
        now_playing_monitor_started_ = true;
        now_playing_subscription_ = media_->subscribe([state = now_playing_async_state_] {
            ShellRuntime* owner = state->owner.load();
            if (!state->alive.load() || owner == nullptr) return;
            owner->request_now_playing_refresh();
        });
        request_now_playing_refresh();
    }

    void apply_now_playing_media(std::optional<services::MediaInfo> info) {
        std::string identity;
        if (info) {
            identity.reserve(
                info->player_bus_name.size() + info->track_id.size() +
                info->title.size() + info->artist.size() + info->album.size() + 8
            );
            identity.append(info->player_bus_name);
            identity.push_back('\x1f');
            if (!info->track_id.empty()) {
                identity.append("track:");
                identity.append(info->track_id);
            } else if (!info->title.empty() || !info->artist.empty() ||
                       !info->album.empty()) {
                // Some browser bridges omit mpris:trackid. Fall back to stable
                // metadata, but deliberately ignore artwork and playback state
                // so late artwork loads and pause/resume cannot duplicate a toast.
                identity.append("metadata:");
                identity.append(info->title);
                identity.push_back('\x1f');
                identity.append(info->artist);
                identity.push_back('\x1f');
                identity.append(info->album);
            } else {
                identity.clear();
            }
        }

        if (!now_playing_seeded_) {
            now_playing_seeded_ = true;
            last_now_playing_identity_ = std::move(identity);
            return;
        }

        if (identity.empty()) {
            last_now_playing_identity_.clear();
            return;
        }
        if (identity == last_now_playing_identity_) return;

        last_now_playing_identity_ = std::move(identity);
        if (!info) return;
        ensure_now_playing_overlay(invocation_monitor_index());
        const std::string title = info->title.empty()
            ? "Unknown track"
            : info->title;
        const std::string artist = info->artist.empty() ? info->album : info->artist;
        now_playing_->show(title, artist);
    }

    void finish_mana_cores_launch(
        std::uint64_t generation,
        std::string current_path,
        int original_workspace,
        bool switched,
        int monitor_index,
        std::string monitor_connector
    ) {
        if (generation != mana_cores_launch_generation_) return;
        mana_cores_launch_pending_ = false;

        if (!switched) {
            restore_mana_cores_chrome();
            std::cerr << "[ManaCores] unable to enter the empty ManaCores workspace\n";
            return;
        }

        // Create and present the ManaCores selector
        if (!mana_cores_selector_) {
            mana_cores_selector_ = std::make_unique<realmheart::mana_core::ManaCoresSelector>();
        }
        const wallpaper::WallpaperOutputTarget wallpaper_target{
            monitor_index,
            monitor_connector
        };
        // Set dismiss callback to restore workspace + bar when selector closes.
        // Keep the connector in wallpaper_target before moving our local copy
        // into the dismiss callback.
        mana_cores_selector_->set_dismiss_callback([
            this,
            original_workspace,
            monitor_connector = std::move(monitor_connector)
        ] {
            handle_mana_cores_dismiss(original_workspace, monitor_connector);
        });
        // Set apply callback to commit the selected wallpaper only to the
        // output that owns this Mana Cores session. The selector is a
        // monitor-local surface; applying from monitor A must never mutate
        // monitor B's wallpaper.
        mana_cores_selector_->set_apply_callback([
            this, wallpaper_target
        ](const std::string& path) {
            if (wallpaper_controller_) {
                wallpaper_controller_->prepare_wallpaper_for_output_async(
                    std::filesystem::path(path),
                    wallpaper_target,
                    [this, path, wallpaper_target](bool success, std::string error_msg) {
                        if (!success) {
                            std::cerr << "[ManaCores] wallpaper prepare failed: " << error_msg << "\n";
                            return;
                        }
                        wallpaper_controller_->commit_prepared_wallpaper_async(
                            [this, path, wallpaper_target](
                                bool success,
                                std::string error_msg
                            ) {
                                if (!success) {
                                    std::cerr << "[ManaCores] wallpaper commit failed: " << error_msg << "\n";
                                } else {
                                    if (services::WallpaperService* service =
                                            utilities_->get_wallpaper_service()) {
                                        if (!wallpaper_target.connector.empty()) {
                                            if (!service->persist_output_path(
                                                    wallpaper_target.connector, path
                                                )) {
                                                std::cerr
                                                    << "[ManaCores] wallpaper changed, but the output-specific path could not be persisted\n";
                                            }
                                        } else {
                                            std::cerr
                                                << "[ManaCores] output connector unavailable; wallpaper override is session-only\n";
                                        }
                                    }
                                    generate_theme_for(path);
                                }
                            }
                        );
                    }
                );
            }
        });

        // Use the stored application reference
        mana_cores_selector_->present(application_, monitor_index);

        // Load wallpapers from the library for the selector
        mana_cores_selector_->load_wallpapers_from_library(std::filesystem::path(current_path));
    }

    void restore_mana_cores_workspace() {
        const int workspace_id = mana_cores_restore_workspace_id_;
        const std::string monitor_connector =
            std::exchange(mana_cores_restore_monitor_connector_, {});
        mana_cores_restore_workspace_id_ = 0;
        ++mana_cores_launch_generation_;
        mana_cores_launch_pending_ = false;

        if (workspace_id <= 0) {
            mana_cores_restore_pending_ = false;
            restore_mana_cores_chrome();
            return;
        }

        mana_cores_restore_pending_ = true;
        const auto async_state = runtime_async_state_;
        const bool posted = core::shared_task_executor().post([
            async_state,
            workspace_id,
            monitor_connector
        ] {
            const bool restored = monitor_connector.empty()
                ? services::HyprlandWorkspaces::switch_to(workspace_id)
                : services::HyprlandWorkspaces::switch_to_on_monitor(
                      workspace_id, monitor_connector
                  );

            struct Payload {
                std::shared_ptr<RuntimeAsyncState> state;
                int workspace_id = 0;
                bool restored = false;
            };
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    ShellRuntime* owner = payload->state->owner.load();
                    if (payload->state->alive.load() && owner != nullptr) {
                        owner->mana_cores_restore_pending_ = false;
                        if (!payload->restored) {
                            std::cerr
                                << "[ManaCores] unable to restore workspace "
                                << payload->workspace_id << '\n';
                        }
                        owner->restore_mana_cores_chrome();
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{async_state, workspace_id, restored},
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        });

        if (!posted) {
            mana_cores_restore_pending_ = false;
            restore_mana_cores_chrome();
        }
    }

    void restore_mana_cores_chrome() {
        if (mana_cores_bar_was_visible_ && state_.bar_visible()) {
            present_all_bars();
        }
        mana_cores_bar_was_visible_ = false;
    }

    void handle_mana_cores_dismiss(
        int original_workspace,
        const std::string& monitor_connector
    ) {
        // Restore the exact output/workspace pair that owned the selector.
        mana_cores_restore_workspace_id_ = original_workspace;
        mana_cores_restore_monitor_connector_ = monitor_connector;
        restore_mana_cores_workspace();
    }

    struct MonitorWallpaperRestore {
        wallpaper::WallpaperOutputTarget target;
        std::filesystem::path path;
    };

    void restore_monitor_wallpapers() {
        monitor_wallpaper_restore_jobs_.clear();
        monitor_wallpaper_restore_index_ = 0;
        if (wallpaper_controller_ == nullptr) return;

        services::WallpaperService* service = utilities_->get_wallpaper_service();
        GdkDisplay* display = gdk_display_get_default();
        GListModel* monitors = display != nullptr
            ? gdk_display_get_monitors(display)
            : nullptr;
        if (service == nullptr || monitors == nullptr) return;

        const guint count = g_list_model_get_n_items(monitors);
        for (guint index = 0; index < count; ++index) {
            const std::string connector = monitor_connector_for_index(
                display, static_cast<int>(index)
            );
            if (connector.empty()) continue;
            const auto path = service->load_output_path(connector);
            if (!path) continue;
            monitor_wallpaper_restore_jobs_.push_back(MonitorWallpaperRestore{
                wallpaper::WallpaperOutputTarget{
                    static_cast<int>(index), connector
                },
                *path
            });
        }
        restore_next_monitor_wallpaper();
    }

    void restore_next_monitor_wallpaper() {
        if (wallpaper_controller_ == nullptr ||
            monitor_wallpaper_restore_index_ >=
                monitor_wallpaper_restore_jobs_.size()) {
            monitor_wallpaper_restore_jobs_.clear();
            monitor_wallpaper_restore_index_ = 0;
            return;
        }

        const MonitorWallpaperRestore job =
            monitor_wallpaper_restore_jobs_[monitor_wallpaper_restore_index_++];
        wallpaper_controller_->prepare_wallpaper_for_output_async(
            job.path,
            job.target,
            [this, job](bool success, std::string error_message) {
                if (!success) {
                    std::cerr
                        << "Unable to restore wallpaper for output "
                        << job.target.connector << ": " << error_message << '\n';
                    restore_next_monitor_wallpaper();
                    return;
                }
                wallpaper_controller_->commit_prepared_wallpaper_async(
                    [this, job](bool commit_success, std::string commit_error) {
                        if (!commit_success) {
                            std::cerr
                                << "Unable to commit wallpaper for output "
                                << job.target.connector << ": "
                                << commit_error << '\n';
                        }
                        restore_next_monitor_wallpaper();
                    }
                );
            }
        );
    }

    using WallpaperRequestCompletion =
        std::function<void(bool, std::string)>;

    void request_wallpaper(
        const std::string& path,
        const char* failure_prefix,
        WallpaperRequestCompletion completion = {}
    ) {
        if (path.empty() || wallpaper_controller_ == nullptr) {
            if (completion) completion(false, "wallpaper controller is unavailable");
            return;
        }
        const auto utilities = utilities_;
        wallpaper_controller_->set_wallpaper_async(
            path,
            [
                this,
                utilities,
                path,
                failure_prefix = std::string(failure_prefix),
                completion = std::move(completion)
            ](bool success, std::string error_message) mutable {
                if (!success) {
                    std::cerr << failure_prefix << ": " << error_message << '\n';
                    if (completion) {
                        completion(false, std::move(error_message));
                    }
                    return;
                }

                if (services::WallpaperService* service = utilities->get_wallpaper_service()) {
                    if (!service->update_state(path)) {
                        std::cerr << "Wallpaper changed, but its path could not be persisted\n";
                    }
                }
                generate_theme_for(path);
                if (completion) completion(true, {});
            }
        );
    }

    void generate_theme_for(const std::string& path) {
        if (path.empty()) return;
        const auto state = runtime_async_state_;
        const auto utilities = utilities_;
        const auto theme_service = theme_service_;
        const std::uint64_t generation = state->theme_generation.fetch_add(1) + 1;
        const bool posted = core::shared_task_executor().post([
            state, utilities, theme_service, path, generation
        ] {
            auto palette = utilities->generate_palette(path);

            struct Payload {
                std::shared_ptr<RuntimeAsyncState> state;
                std::shared_ptr<services::ThemeService> theme_service;
                std::uint64_t generation = 0;
                std::optional<services::Palette> palette;
            };
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    if (!payload->state->alive.load() ||
                        payload->state->owner.load() == nullptr ||
                        payload->state->theme_generation.load() != payload->generation) {
                        return G_SOURCE_REMOVE;
                    }
                    if (payload->palette) {
                        payload->theme_service->update_palette(
                            std::move(*payload->palette)
                        );
                    } else {
                        std::cerr << "[Theme] Keeping the current palette because generation failed\n";
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{state, theme_service, generation, std::move(palette)},
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        });
        if (!posted) {
            std::cerr << "[Theme] Worker queue unavailable; keeping the current palette\n";
        }
    }

    [[nodiscard]] int invocation_monitor_index() const {
        return focused_monitor_index(gdk_display_get_default());
    }

    [[nodiscard]] bar::VerticalBar* bar_for_monitor(int monitor_index) const {
        if (monitor_index <= 0) return bar_.get();
        const std::size_t offset = static_cast<std::size_t>(monitor_index - 1);
        return offset < secondary_bars_.size()
            ? secondary_bars_[offset].get()
            : bar_.get();
    }

    [[nodiscard]] bar::VerticalBar* active_bar() const {
        return bar_for_monitor(active_monitor_index_);
    }

    void hide_all_bars() {
        if (bar_ != nullptr) gtk_widget_set_visible(bar_->get_window(), FALSE);
        for (const auto& bar : secondary_bars_) {
            if (bar != nullptr) gtk_widget_set_visible(bar->get_window(), FALSE);
        }
    }

    void present_all_bars() {
        if (bar_ != nullptr) {
            bar_->refresh();
            gtk_window_present(GTK_WINDOW(bar_->get_window()));
        }
        for (const auto& bar : secondary_bars_) {
            if (bar == nullptr) continue;
            bar->refresh();
            gtk_window_present(GTK_WINDOW(bar->get_window()));
        }
    }

    void ensure_toast_overlay(int monitor_index) {
        if (toast_ != nullptr && toast_monitor_index_ != monitor_index) {
            toast_.reset();
        }
        if (!toast_) {
            toast_monitor_index_ = monitor_index;
            toast_ = std::make_unique<NotificationToast>(application_, monitor_index);
        }
    }

    void ensure_now_playing_overlay(int monitor_index) {
        if (now_playing_ != nullptr && now_playing_monitor_index_ != monitor_index) {
            now_playing_.reset();
        }
        if (now_playing_) return;
        now_playing_monitor_index_ = monitor_index;
        now_playing_ = std::make_unique<NowPlayingOverlay>(application_, monitor_index);
        now_playing_->set_system_osd_visible(system_osd_visible_);
    }

    void ensure_osd_overlay(int monitor_index) {
        if (osd_ != nullptr && osd_monitor_index_ != monitor_index) {
            osd_.reset();
            system_osd_visible_ = false;
        }
        if (osd_) return;
        osd_monitor_index_ = monitor_index;
        osd_ = std::make_unique<OSDOverlay>(
            application_,
            [this](bool visible) {
                system_osd_visible_ = visible;
                if (now_playing_) {
                    now_playing_->set_system_osd_visible(visible);
                }
            },
            monitor_index
        );
    }

    void ensure_notes_overlay(int monitor_index) {
        ensure_core_initialized();
        if (!notes_service_) {
            notes_service_ = std::make_unique<services::NotesService>();
        }
        if (notes_overlay_ != nullptr && notes_monitor_index_ != monitor_index) {
            notes_overlay_.reset();
        }
        if (!notes_overlay_) {
            notes_monitor_index_ = monitor_index;
            notes_overlay_ = std::make_unique<NotesOverlay>(
                application_,
                notes_service_.get(),
                monitor_index
            );
        }
    }

    void ensure_sidebar_initialized(int monitor_index) {
        ensure_core_initialized();
        if (sidebar_ != nullptr && sidebar_monitor_index_ != monitor_index) {
            if (sidebar_tick_id_ != 0) {
                gtk_widget_remove_tick_callback(sidebar_->get_window(), sidebar_tick_id_);
                sidebar_tick_id_ = 0;
                sidebar_last_frame_time_ = 0;
            }
            sidebar_.reset();
            if (sidebar_backdrop_ != nullptr) {
                gtk_window_destroy(sidebar_backdrop_);
                sidebar_backdrop_ = nullptr;
            }
        }
        if (!sidebar_) {
            sidebar_monitor_index_ = monitor_index;
            sidebar_ = std::make_unique<sidebar::RightSidebar>(
                application_,
                notification_history_,
                [this, monitor_index](double value) {
                    show_osd_volume_value(value, monitor_index);
                },
                [this, monitor_index](double value) {
                    show_osd_brightness_value(value, monitor_index);
                },
                monitor_index
            );
            gtk_widget_set_visible(sidebar_->get_window(), FALSE);
        }
        if (sidebar_backdrop_ == nullptr) {
            sidebar_backdrop_ = GTK_WINDOW(
                gtk_application_window_new(application_)
            );
            gtk_window_set_decorated(sidebar_backdrop_, FALSE);
            // Opposite layer-shell anchors only stretch the window when GTK
            // permits it to resize. A non-resizable window stays at GTK's
            // fallback natural size (observed as 200x200), so its input region
            // can never cover the monitor.
            gtk_window_set_resizable(sidebar_backdrop_, TRUE);
            gtk_widget_add_css_class(
                GTK_WIDGET(sidebar_backdrop_),
                "realmheart-sidebar-backdrop-window"
            );

            LayerSurfaceSpec backdrop_spec;
            backdrop_spec.surface_namespace = "realmheart-sidebar-backdrop";
            backdrop_spec.layer = LayerSurfaceLevel::Overlay;
            backdrop_spec.keyboard_mode = LayerKeyboardMode::None;
            backdrop_spec.anchor_left = true;
            backdrop_spec.anchor_right = true;
            backdrop_spec.anchor_top = true;
            backdrop_spec.anchor_bottom = true;
            backdrop_spec.monitor_index = monitor_index;
            apply_layer_surface(sidebar_backdrop_, backdrop_spec);

            GtkWidget* dismiss_button = gtk_button_new();
            gtk_button_set_has_frame(GTK_BUTTON(dismiss_button), FALSE);
            gtk_widget_set_focusable(dismiss_button, FALSE);
            gtk_widget_set_hexpand(dismiss_button, TRUE);
            gtk_widget_set_vexpand(dismiss_button, TRUE);
            gtk_widget_add_css_class(
                dismiss_button,
                "realmheart-sidebar-backdrop-button"
            );
            g_signal_connect(
                dismiss_button,
                "clicked",
                G_CALLBACK(+[](GtkButton*, gpointer data) {
                    sidebar_input_debug("backdrop button: clicked");
                    auto* runtime = static_cast<ShellRuntime*>(data);
                    const int owner = runtime->sidebar_monitor_index_;
                    runtime->toggle_right_sidebar_on_monitor(
                        owner >= 0 ? owner : runtime->invocation_monitor_index()
                    );
                }),
                this
            );

            GtkGesture* backdrop_probe = gtk_gesture_click_new();
            gtk_event_controller_set_propagation_phase(
                GTK_EVENT_CONTROLLER(backdrop_probe),
                GTK_PHASE_CAPTURE
            );
            g_signal_connect(
                backdrop_probe,
                "pressed",
                G_CALLBACK(+[](
                    GtkGestureClick*,
                    int,
                    double x,
                    double y,
                    gpointer
                ) {
                    sidebar_input_debug(
                        "backdrop surface: pointer pressed at ", x, ",", y
                    );
                }),
                nullptr
            );
            gtk_widget_add_controller(
                GTK_WIDGET(sidebar_backdrop_),
                GTK_EVENT_CONTROLLER(backdrop_probe)
            );

            GtkWidget* backdrop_overlay = gtk_overlay_new();
            gtk_widget_set_hexpand(backdrop_overlay, TRUE);
            gtk_widget_set_vexpand(backdrop_overlay, TRUE);
            gtk_overlay_set_child(
                GTK_OVERLAY(backdrop_overlay),
                dismiss_button
            );

            GtkWidget* commit_fill = gtk_drawing_area_new();
            gtk_widget_set_hexpand(commit_fill, TRUE);
            gtk_widget_set_vexpand(commit_fill, TRUE);
            gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(commit_fill),
                draw_backdrop_commit_fill,
                nullptr,
                nullptr
            );
            gtk_widget_set_halign(commit_fill, GTK_ALIGN_FILL);
            gtk_widget_set_valign(commit_fill, GTK_ALIGN_FILL);
            gtk_widget_set_can_target(commit_fill, FALSE);
            gtk_widget_set_focusable(commit_fill, FALSE);
            gtk_overlay_add_overlay(
                GTK_OVERLAY(backdrop_overlay),
                commit_fill
            );
            gtk_overlay_set_measure_overlay(
                GTK_OVERLAY(backdrop_overlay),
                commit_fill,
                FALSE
            );

            gtk_window_set_child(sidebar_backdrop_, backdrop_overlay);
            gtk_widget_set_visible(GTK_WIDGET(sidebar_backdrop_), FALSE);
        }
    }

    void ensure_launcher_initialized(int monitor_index) {
        ensure_core_initialized();
        if (!launcher_service_) {
            launcher_service_ = std::make_unique<services::LauncherService>();
        }
        if (!command_receipts_) {
            command_receipts_ = std::make_unique<CommandReceiptOverlay>();
        }
        if (launcher_overlay_ != nullptr && launcher_monitor_index_ != monitor_index) {
            launcher_overlay_->hide();
            launcher_overlay_.reset();
        }
        if (!launcher_overlay_) {
            launcher_monitor_index_ = monitor_index;
            launcher_overlay_ = std::make_unique<LauncherOverlay>(
                application_,
                *launcher_service_,
                *utilities_->get_wallpaper_service(),
                *command_receipts_,
                monitor_index
            );
        }
    }

    void apply_hotspot_geometry(GtkWindow* window) {
        if (window == nullptr) return;
        auto it = std::find_if(
            hotspots_.begin(),
            hotspots_.end(),
            [window](const MonitorHotspot& hotspot) {
                return hotspot.window == window;
            }
        );
        if (it == hotspots_.end()) return;

        const auto placement = sidebar::sidebar_placement_for(
            GTK_WIDGET(window),
            it->monitor_index
        );
        if (placement.monitor_height <= 0) return;

        gtk_window_set_default_size(
            window,
            placement.frame_layout.hotspot_hit_width,
            placement.height
        );
        gtk_layer_set_margin(
            window,
            GTK_LAYER_SHELL_EDGE_TOP,
            placement.top_margin
        );
        if (it->button != nullptr) {
            gtk_widget_set_size_request(
                it->button,
                placement.frame_layout.hotspot_hit_width,
                placement.height
            );
        }
        if (it->commit_pixel != nullptr) {
            gtk_drawing_area_set_content_height(
                GTK_DRAWING_AREA(it->commit_pixel),
                placement.height
            );
        }
    }

    void destroy_monitor_hotspots() {
        for (auto& hotspot : hotspots_) {
            if (hotspot.window != nullptr) {
                gtk_window_destroy(hotspot.window);
                hotspot.window = nullptr;
            }
        }
        hotspots_.clear();
    }

    void create_monitor_hotspot(int monitor_index) {
        MonitorHotspot hotspot;
        hotspot.monitor_index = monitor_index;
        hotspot.window = GTK_WINDOW(gtk_application_window_new(application_));
        gtk_window_set_decorated(hotspot.window, FALSE);
        gtk_window_set_resizable(hotspot.window, FALSE);
        gtk_widget_add_css_class(
            GTK_WIDGET(hotspot.window),
            "realmheart-right-hotspot-window"
        );

        const auto placement = sidebar::sidebar_placement_for(
            GTK_WIDGET(hotspot.window),
            monitor_index
        );
        gtk_window_set_default_size(
            hotspot.window,
            placement.frame_layout.hotspot_hit_width,
            placement.height
        );

        LayerSurfaceSpec spec;
        spec.surface_namespace = "realmheart-right-hotspot";
        spec.layer = LayerSurfaceLevel::Overlay;
        spec.anchor_right = true;
        spec.anchor_top = true;
        spec.anchor_bottom = false;
        spec.margin_top = placement.top_margin;
        spec.monitor_index = monitor_index;
        apply_layer_surface(hotspot.window, spec);
        g_signal_connect(
            hotspot.window,
            "realize",
            G_CALLBACK(+[](GtkWidget* widget, gpointer data) {
                static_cast<ShellRuntime*>(data)->apply_hotspot_geometry(
                    GTK_WINDOW(widget)
                );
            }),
            this
        );

        GtkWidget* button = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
        gtk_widget_set_focusable(button, FALSE);
        gtk_widget_set_hexpand(button, TRUE);
        gtk_widget_set_vexpand(button, TRUE);
        gtk_widget_set_size_request(
            button,
            placement.frame_layout.hotspot_hit_width,
            placement.height
        );
        hotspot.button = button;
        gtk_widget_add_css_class(button, "realmheart-right-hotspot-button");
        g_object_set_data(
            G_OBJECT(button),
            "realmheart-monitor-index",
            GINT_TO_POINTER(monitor_index)
        );
        g_signal_connect(
            button,
            "clicked",
            G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
                const int target = GPOINTER_TO_INT(g_object_get_data(
                    G_OBJECT(clicked),
                    "realmheart-monitor-index"
                ));
                sidebar_input_debug(
                    "hotspot button: clicked monitor=", target
                );
                static_cast<ShellRuntime*>(data)->toggle_right_sidebar_on_monitor(
                    target
                );
            }),
            this
        );

        GtkWidget* overlay = gtk_overlay_new();
        gtk_widget_set_hexpand(overlay, TRUE);
        gtk_widget_set_vexpand(overlay, TRUE);
        gtk_overlay_set_child(GTK_OVERLAY(overlay), button);

        GtkWidget* commit_pixel = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(commit_pixel), 1);
        gtk_drawing_area_set_content_height(
            GTK_DRAWING_AREA(commit_pixel),
            placement.height
        );
        hotspot.commit_pixel = commit_pixel;
        gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(commit_pixel),
            draw_hotspot_commit_pixel,
            nullptr,
            nullptr
        );
        gtk_widget_set_halign(commit_pixel, GTK_ALIGN_END);
        gtk_widget_set_valign(commit_pixel, GTK_ALIGN_FILL);
        gtk_widget_set_can_target(commit_pixel, FALSE);
        gtk_widget_set_focusable(commit_pixel, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), commit_pixel);
        gtk_overlay_set_measure_overlay(
            GTK_OVERLAY(overlay),
            commit_pixel,
            FALSE
        );

        gtk_window_set_child(hotspot.window, overlay);
        hotspots_.push_back(hotspot);
        gtk_window_present(hotspot.window);
        enforce_hotspot_input_region(hotspot.window);
    }

    void rebuild_monitor_surfaces() {
        GdkDisplay* display = gdk_display_get_default();
        const int count = std::max(monitor_count(display), 1);

        // A wl_output appearing/disappearing while native Broken Seal is up
        // invalidates the exact set of fullscreen security surfaces. Keep the
        // custom lockscreen for normal multi-monitor use, but fail closed to
        // hyprlock for the rare hotplug-while-locked case. Hidden lock surfaces
        // can simply be rebuilt lazily on the next lock.
        const bool native_lock_visible =
            lock_surface_ != nullptr && lock_surface_->visible();
        if (lock_choreography_pending_) {
            // Let the in-flight workspace worker return its restore points
            // first; finish_lock_choreography() will then fail closed to
            // hyprlock without losing the workspaces we must restore.
            lock_topology_dirty_ = true;
        } else if (native_lock_visible) {
            lock_topology_dirty_ = true;
            fallback_to_hyprlock(
                "monitor topology changed while Broken Seal was active"
            );
        } else if (lock_surface_ != nullptr || !lock_mirror_surfaces_.empty()) {
            hide_native_lock_surfaces_immediately();
            lock_mirror_surfaces_.clear();
            lock_surface_.reset();
            lock_monitor_index_ = -1;
            lock_topology_dirty_ = false;
        }

        // A topology change can arrive mid-sidebar transition.  Remove the
        // frame callback before destroying/recreating the monitor-bound
        // surface so a stale tick can never dereference the previous sidebar.
        if (sidebar_tick_id_ != 0 && sidebar_ != nullptr) {
            gtk_widget_remove_tick_callback(sidebar_->get_window(), sidebar_tick_id_);
            sidebar_tick_id_ = 0;
            sidebar_last_frame_time_ = 0;
        }

        // Short-lived fullscreen/transient surfaces cannot retain a removed
        // wl_output. Close them before recreating monitor-owned shell chrome.
        power_menu_process_.close();
        if (mana_cores_selector_ != nullptr && mana_cores_selector_->is_visible()) {
            mana_cores_selector_->request_dismiss();
        }

        if (state_.right_sidebar_visible()) {
            if (sidebar_ != nullptr) {
                gtk_widget_set_visible(sidebar_->get_window(), FALSE);
            }
            if (sidebar_backdrop_ != nullptr) {
                gtk_widget_set_visible(GTK_WIDGET(sidebar_backdrop_), FALSE);
            }
            state_.set_right_sidebar_visible(false);
            sidebar_transition_.snap_hidden();
        }
        sidebar_.reset();
        if (sidebar_backdrop_ != nullptr) {
            gtk_window_destroy(sidebar_backdrop_);
            sidebar_backdrop_ = nullptr;
        }

        workspace_overview_.reset();
        launcher_overlay_.reset();
        notes_overlay_.reset();
        toast_.reset();
        osd_.reset();
        now_playing_.reset();
        overview_monitor_index_ = -1;
        launcher_monitor_index_ = -1;
        notes_monitor_index_ = -1;
        sidebar_monitor_index_ = -1;
        toast_monitor_index_ = -1;
        osd_monitor_index_ = -1;
        now_playing_monitor_index_ = -1;
        system_osd_visible_ = false;
        active_monitor_index_ = std::clamp(active_monitor_index_, 0, count - 1);
        monitor_workspace_snapshots_.resize(static_cast<std::size_t>(count));

        destroy_monitor_hotspots();
        secondary_bars_.clear();
        bar_.reset();

        auto make_bar = [this](int monitor_index) {
            return std::make_unique<bar::VerticalBar>(
                application_,
                notification_history_,
                *battery_,
                *media_,
                [this, monitor_index] {
                    toggle_right_sidebar_on_monitor(monitor_index);
                },
                [this, monitor_index] {
                    launch_launcher_on_monitor(monitor_index);
                },
                [this, monitor_index] {
                    toggle_workspace_overview_on_monitor(monitor_index);
                },
                [this, monitor_index](double origin_x, double origin_y) {
                    open_logout_menu_on_monitor(monitor_index, origin_x, origin_y);
                },
                [this, monitor_index](services::WorkspaceSnapshot snapshot) {
                    apply_workspace_snapshot(monitor_index, std::move(snapshot));
                },
                monitor_index
            );
        };

        bar_ = make_bar(0);
        secondary_bars_.reserve(static_cast<std::size_t>(std::max(count - 1, 0)));
        for (int index = 1; index < count; ++index) {
            secondary_bars_.push_back(make_bar(index));
        }

        hotspots_.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            create_monitor_hotspot(index);
        }
        apply_bar_visibility();
    }

    void schedule_monitor_surface_rebuild() {
        if (monitor_rebuild_idle_id_ != 0) return;
        monitor_rebuild_idle_id_ = g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<ShellRuntime*>(data);
                self->monitor_rebuild_idle_id_ = 0;
                self->rebuild_monitor_surfaces();
                return G_SOURCE_REMOVE;
            },
            this,
            nullptr
        );
    }

    void ensure_monitor_watch() {
        if (monitor_model_ != nullptr) return;
        GdkDisplay* display = gdk_display_get_default();
        if (display == nullptr) return;
        monitor_model_ = gdk_display_get_monitors(display);
        if (monitor_model_ == nullptr) return;
        monitor_model_signal_id_ = g_signal_connect(
            monitor_model_,
            "items-changed",
            G_CALLBACK(+[](
                GListModel*, guint, guint, guint, gpointer data
            ) {
                static_cast<ShellRuntime*>(data)->schedule_monitor_surface_rebuild();
            }),
            this
        );
    }

    void ensure_monitor_surfaces() {
        ensure_monitor_watch();
        const int expected = std::max(monitor_count(gdk_display_get_default()), 1);
        const int current = bar_ == nullptr
            ? 0
            : 1 + static_cast<int>(secondary_bars_.size());
        if (current == expected && static_cast<int>(hotspots_.size()) == expected) {
            return;
        }
        rebuild_monitor_surfaces();
    }

    void ensure_core_initialized() {
        if (!theme_styles_) {
            theme_styles_ = std::make_unique<ThemeStyles>(theme_service_);
        }
        if (!audio_monitor_) {
            audio_monitor_ = std::make_unique<services::AudioMonitor>(
                [this](const services::AudioState& audio) {
                    // Muting does not change PipeWire's stored volume. Showing
                    // mute as 0% makes a late monitor refresh overwrite the
                    // real level that was just displayed by the volume action.
                    // Keep the OSD percentage tied to the actual volume level;
                    // mute presentation can be handled independently by icon
                    // state when the OSD gains that capability.
                    ++runtime_async_state_->volume_generation;
                    const double percent = std::clamp(
                        audio.volume * 100.0,
                        0.0,
                        100.0
                    );
                    show_osd_volume_value(percent);
                }
            );
            audio_monitor_->start();
        }
        start_now_playing_monitor();
        ensure_monitor_surfaces();
        if (!wallpaper_controller_) {
            wallpaper_controller_ = std::make_unique<wallpaper::WallpaperController>(
                application_,
                requested_wallpaper_backend_
            );
            std::string error_message;
            if (!wallpaper_controller_->initialize(&error_message)) {
                std::cerr << "Unable to initialize wallpaper backend: "
                          << error_message << '\n';
            }
        }
    }

    void apply_right_sidebar_surface_effect() {
        if (sidebar_ == nullptr) return;
        sidebar_->set_surface_effect(
            kSidebarSurfaceEffect,
            sidebar_transition_.progress()
        );
    }

    [[nodiscard]] bool advance_right_sidebar_frame(GdkFrameClock* frame_clock) {
        const gint64 frame_time = gdk_frame_clock_get_frame_time(frame_clock);
        double elapsed = 1.0 / 60.0;
        if (sidebar_last_frame_time_ != 0) {
            elapsed = static_cast<double>(frame_time - sidebar_last_frame_time_) /
                1'000'000.0;
        }
        sidebar_last_frame_time_ = frame_time;
        elapsed = std::clamp(elapsed, 1.0 / 240.0, 0.05);

        const bool still_running = sidebar_transition_.advance(elapsed);
        apply_right_sidebar_surface_effect();
        if (!still_running &&
            sidebar_transition_.state() == effects::TransitionState::Hidden) {
            finish_right_sidebar_hide_if_ready();
        }
        return still_running;
    }

    void schedule_right_sidebar_frame() {
        if (sidebar_ == nullptr || sidebar_tick_id_ != 0 ||
            !sidebar_transition_.active()) {
            return;
        }

        sidebar_tick_id_ = gtk_widget_add_tick_callback(
            sidebar_->get_window(),
            +[](GtkWidget*, GdkFrameClock* frame_clock, gpointer data) -> gboolean {
                auto* runtime = static_cast<ShellRuntime*>(data);
                if (runtime->advance_right_sidebar_frame(frame_clock)) {
                    return G_SOURCE_CONTINUE;
                }
                runtime->sidebar_tick_id_ = 0;
                runtime->sidebar_last_frame_time_ = 0;
                return G_SOURCE_REMOVE;
            },
            this,
            nullptr
        );
    }

    void finish_right_sidebar_hide_if_ready() {
        if (state_.right_sidebar_visible() ||
            sidebar_transition_.state() != effects::TransitionState::Hidden ||
            !sidebar_character_exit_complete_) {
            return;
        }

        gtk_widget_set_visible(sidebar_->get_window(), FALSE);
    }

    void apply_right_sidebar_visibility() {
        GtkWidget* window = sidebar_->get_window();
        if (sidebar_transition_.target_visible()) {
            sidebar_input_debug("visibility: presenting backdrop then sidebar");
            sidebar_character_exit_complete_ = false;
            gtk_widget_set_sensitive(window, TRUE);
            apply_right_sidebar_surface_effect();

            // Use an Overlay backdrop with a carved input region instead of
            // relying on cross-layer stacking. Only clicks outside the sidebar
            // are accepted by this surface.
            gtk_window_present(sidebar_backdrop_);
            const auto placement = sidebar::sidebar_placement_for(
                GTK_WIDGET(sidebar_backdrop_),
                sidebar_monitor_index_
            );
            enforce_sidebar_backdrop_input_region(
                sidebar_backdrop_,
                placement
            );

            sidebar_->refresh();
            sidebar_->apply_geometry();
            gtk_window_present(GTK_WINDOW(window));
            sidebar_->animate_character_in();
            schedule_right_sidebar_frame();
            return;
        }

        sidebar_input_debug("visibility: animating character out, then hiding sidebar");
        gtk_widget_set_visible(GTK_WIDGET(sidebar_backdrop_), FALSE);
        gtk_widget_set_sensitive(window, FALSE);
        sidebar_character_exit_complete_ = false;

        // The surface transition and Tessia's exit are independent. The none
        // effect reaches Hidden immediately today, while this completion gate
        // also supports a future animated surface without allowing either side
        // to hide a sidebar that has already been reopened.
        if (!sidebar_->animate_character_out([this] {
                sidebar_character_exit_complete_ = true;
                finish_right_sidebar_hide_if_ready();
            })) {
            sidebar_character_exit_complete_ = true;
        }
        schedule_right_sidebar_frame();
        finish_right_sidebar_hide_if_ready();
    }

    void apply_bar_visibility() {
        if (state_.bar_visible()) {
            present_all_bars();
        } else {
            hide_all_bars();
        }
    }

    GtkApplication* application_ = nullptr;
    wallpaper::WallpaperBackendType requested_wallpaper_backend_ =
        wallpaper::WallpaperBackendType::Gtk;

    services::NotificationHistory notification_history_;
    services::NotificationServer notification_server_;
    services::NotificationDaemon notification_daemon_;

    std::shared_ptr<services::ThemeService> theme_service_;
    std::shared_ptr<services::UtilityManager> utilities_;
    std::unique_ptr<services::SessionManager> session_;
    std::unique_ptr<services::BatteryService> battery_;
    std::shared_ptr<services::MediaService> media_;
    services::MediaService::Subscription now_playing_subscription_;
    std::shared_ptr<NowPlayingAsyncState> now_playing_async_state_ =
        std::make_shared<NowPlayingAsyncState>();
    std::shared_ptr<RuntimeAsyncState> runtime_async_state_ =
        std::make_shared<RuntimeAsyncState>();
    bool now_playing_monitor_started_ = false;
    bool now_playing_seeded_ = false;
    bool system_osd_visible_ = false;
    std::string last_now_playing_identity_;
    std::unique_ptr<services::NotesService> notes_service_;
    std::unique_ptr<services::LauncherService> launcher_service_;

    std::unique_ptr<ThemeStyles> theme_styles_;
    std::unique_ptr<NotesOverlay> notes_overlay_;
    int notes_monitor_index_ = -1;
    std::unique_ptr<NotificationToast> toast_;
    int toast_monitor_index_ = -1;
    std::unique_ptr<NowPlayingOverlay> now_playing_;
    int now_playing_monitor_index_ = -1;
    std::unique_ptr<OSDOverlay> osd_;
    int osd_monitor_index_ = -1;
    std::unique_ptr<services::AudioMonitor> audio_monitor_;
    std::unique_ptr<bar::VerticalBar> bar_;
    std::vector<std::unique_ptr<bar::VerticalBar>> secondary_bars_;
    std::vector<MonitorHotspot> hotspots_;
    GListModel* monitor_model_ = nullptr;
    gulong monitor_model_signal_id_ = 0;
    guint monitor_rebuild_idle_id_ = 0;
    int active_monitor_index_ = 0;
    GtkWindow* sidebar_backdrop_ = nullptr;
    std::unique_ptr<sidebar::RightSidebar> sidebar_;
    int sidebar_monitor_index_ = -1;
    guint right_sidebar_prewarm_id_ = 0;
    effects::TransitionTimeline sidebar_transition_{{0.22, 0.16}};
    guint sidebar_tick_id_ = 0;
    gint64 sidebar_last_frame_time_ = 0;
    bool sidebar_character_exit_complete_ = true;
    std::unique_ptr<CommandReceiptOverlay> command_receipts_;
    std::unique_ptr<LauncherOverlay> launcher_overlay_;
    int launcher_monitor_index_ = -1;
    std::unique_ptr<workspace::WorkspaceOverviewOverlay> workspace_overview_;
    int overview_monitor_index_ = -1;
    guint workspace_overview_prewarm_id_ = 0;
    std::unique_ptr<lockscreen::LockSurface> lock_surface_;
    std::vector<std::unique_ptr<lockscreen::LockSurface>> lock_mirror_surfaces_;
    int lock_monitor_index_ = -1;
    // Lock choreography state (mana-core style workspace slide).
    bool lock_choreography_pending_ = false;
    bool lock_choreography_switched_ = false;
    bool lock_choreography_bar_was_visible_ = false;
    std::vector<LockRestorePoint> lock_restore_points_;
    std::uint64_t lock_choreography_generation_ = 0;
    bool lock_topology_dirty_ = false;
    bool lock_hyprlock_fallback_active_ = false;
    services::WorkspaceSnapshot workspace_snapshot_;
    std::vector<services::WorkspaceSnapshot> monitor_workspace_snapshots_;
    powermenu::PowerMenuProcess power_menu_process_;
    std::unique_ptr<realmheart::mana_core::ManaCoresSelector> mana_cores_selector_;
    bool mana_cores_launch_pending_ = false;
    bool mana_cores_restore_pending_ = false;
    bool mana_cores_bar_was_visible_ = false;
    int mana_cores_deferred_toggle_count_ = 0;
    int mana_cores_restore_workspace_id_ = 0;
    std::string mana_cores_restore_monitor_connector_;
    std::uint64_t mana_cores_launch_generation_ = 0;
    std::unique_ptr<wallpaper::WallpaperController> wallpaper_controller_;
    std::vector<MonitorWallpaperRestore> monitor_wallpaper_restore_jobs_;
    std::size_t monitor_wallpaper_restore_index_ = 0;

    ShellState state_;
};

namespace {

void activate_shell(GtkApplication*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->activate();
}

void toggle_right_sidebar_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_right_sidebar();
}

void show_osd_volume_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->show_osd_volume();
}

void show_osd_brightness_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->show_osd_brightness();
}

void toggle_bar_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_bar();
}

void toggle_character_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_character();
}

void set_character_hair_mode_action(
    GSimpleAction*,
    GVariant* parameter,
    gpointer user_data
) {
    if (parameter == nullptr) return;
    static_cast<ShellRuntime*>(user_data)->set_character_hair_mode(
        g_variant_get_string(parameter, nullptr)
    );
}

void take_screenshot_full_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->take_screenshot_full();
}

void take_screenshot_area_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->take_screenshot_area();
}

void extract_ocr_area_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->extract_ocr_area();
}

void launch_launcher_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->launch_launcher();
}

void launch_launcher_query_action(
    GSimpleAction*,
    GVariant* parameter,
    gpointer user_data
) {
    if (parameter == nullptr) return;
    static_cast<ShellRuntime*>(user_data)->launch_launcher_query(
        g_variant_get_string(parameter, nullptr)
    );
}

void toggle_workspace_overview_action(
    GSimpleAction*,
    GVariant*,
    gpointer user_data
) {
    static_cast<ShellRuntime*>(user_data)->toggle_workspace_overview();
}

void toggle_mana_cores_action(
    GSimpleAction*,
    GVariant*,
    gpointer user_data
) {
    static_cast<ShellRuntime*>(user_data)->toggle_mana_cores();
}

void set_wallpaper_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->set_wallpaper();
}

void set_wallpaper_path_action(GSimpleAction*, GVariant* parameter, gpointer user_data) {
    if (parameter == nullptr) return;
    static_cast<ShellRuntime*>(user_data)->set_wallpaper(
        g_variant_get_string(parameter, nullptr)
    );
}

void set_wallpaper_backend_action(GSimpleAction*, GVariant* parameter, gpointer user_data) {
    if (parameter == nullptr) return;
    static_cast<ShellRuntime*>(user_data)->switch_wallpaper_backend(
        g_variant_get_string(parameter, nullptr)
    );
}

void generate_theme_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->generate_theme();
}

void start_recording_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->start_recording();
}

void stop_recording_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->stop_recording();
}

void toggle_notes_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_notes();
}

void lock_session_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->lock_session();
}

void open_logout_menu_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->open_logout_menu();
}

void restart_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->restart();
}

void quit_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->quit();
}

constexpr GActionEntry kShellActions[] = {
    {"sidebar-right-toggle", toggle_right_sidebar_action, nullptr, nullptr, nullptr, {}},
    {"bar-toggle", toggle_bar_action, nullptr, nullptr, nullptr, {}},
    {"character-toggle", toggle_character_action, nullptr, nullptr, nullptr, {}},
    {"character-hair-mode", set_character_hair_mode_action, "s", nullptr, nullptr, {}},
    {"osd-volume", show_osd_volume_action, nullptr, nullptr, nullptr, {}},
    {"osd-brightness", show_osd_brightness_action, nullptr, nullptr, nullptr, {}},
    {"lock-session", lock_session_action, nullptr, nullptr, nullptr, {}},
    {"logout-menu", open_logout_menu_action, nullptr, nullptr, nullptr, {}},
    {"restart", restart_action, nullptr, nullptr, nullptr, {}},
    {"screenshot-full", take_screenshot_full_action, nullptr, nullptr, nullptr, {}},
    {"screenshot-area", take_screenshot_area_action, nullptr, nullptr, nullptr, {}},
    {"extract-ocr", extract_ocr_area_action, nullptr, nullptr, nullptr, {}},
    {"start-recording", start_recording_action, nullptr, nullptr, nullptr, {}},
    {"stop-recording", stop_recording_action, nullptr, nullptr, nullptr, {}},
    {"toggle-notes", toggle_notes_action, nullptr, nullptr, nullptr, {}},
    {"set-wallpaper", set_wallpaper_action, nullptr, nullptr, nullptr, {}},
    {"set-wallpaper-path", set_wallpaper_path_action, "s", nullptr, nullptr, {}},
    {"set-wallpaper-backend", set_wallpaper_backend_action, "s", nullptr, nullptr, {}},
    {"generate-theme", generate_theme_action, nullptr, nullptr, nullptr, {}},
    {"launch-launcher", launch_launcher_action, nullptr, nullptr, nullptr, {}},
    {"launch-launcher-query", launch_launcher_query_action, "s", nullptr, nullptr, {}},
    {"workspace-overview-toggle", toggle_workspace_overview_action, nullptr, nullptr, nullptr, {}},
    {"mana-cores-toggle", toggle_mana_cores_action, nullptr, nullptr, nullptr, {}},
    {"quit", quit_action, nullptr, nullptr, nullptr, {}},
};

} // namespace

int run_shell(wallpaper::WallpaperBackendType wallpaper_backend) {
    GtkApplication* application = gtk_application_new(
        realmheart::core::shell_application_id().data(),
        G_APPLICATION_DEFAULT_FLAGS
    );
    auto runtime = std::make_unique<ShellRuntime>(application, wallpaper_backend);

    // Safety: a previous session that died while the lockscreen bind jail
    // was active would leave every compositor keybind dead. Reset the submap
    // unconditionally at startup.
    services::HyprlandWorkspaces::set_submap("reset");

    g_action_map_add_action_entries(
        G_ACTION_MAP(application),
        kShellActions,
        static_cast<gint>(sizeof(kShellActions) / sizeof(kShellActions[0])),
        runtime.get()
    );
    g_signal_connect(
        application,
        "activate",
        G_CALLBACK(activate_shell),
        runtime.get()
    );

    const int status = g_application_run(G_APPLICATION(application), 0, nullptr);

    // Controllers own GTK windows and callbacks; destroy them while the
    // GtkApplication/display are still valid.
    runtime.reset();
    g_object_unref(application);
    return status;
}

int run_shell_lifetime_stress(
    wallpaper::WallpaperBackendType wallpaper_backend,
    int iterations
) {
    if (iterations <= 0) return 2;

    const auto config_root = std::filesystem::temp_directory_path() /
        ("realmheart-lifetime-stress-" + std::to_string(::getpid()));
    std::error_code filesystem_error;
    std::filesystem::remove_all(config_root, filesystem_error);
    filesystem_error.clear();
    std::filesystem::create_directories(config_root, filesystem_error);
    if (filesystem_error) {
        std::cerr << "Unable to create lifetime-stress config directory: "
                  << filesystem_error.message() << '\n';
        return 1;
    }
    g_setenv("XDG_CONFIG_HOME", config_root.c_str(), TRUE);

    const auto drain_main_context = [](std::chrono::milliseconds duration) {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        do {
            while (g_main_context_pending(nullptr)) {
                g_main_context_iteration(nullptr, FALSE);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
    };

    for (int iteration = 0; iteration < iterations; ++iteration) {
        GtkApplication* application = gtk_application_new(
            nullptr,
            G_APPLICATION_NON_UNIQUE
        );
        GError* registration_error = nullptr;
        if (!g_application_register(
                G_APPLICATION(application), nullptr, &registration_error
            )) {
            std::cerr << "Lifetime-stress application registration failed: "
                      << (registration_error != nullptr
                              ? registration_error->message
                              : "unknown error")
                      << '\n';
            g_clear_error(&registration_error);
            g_object_unref(application);
            std::filesystem::remove_all(config_root, filesystem_error);
            return 1;
        }

        auto runtime = std::make_unique<ShellRuntime>(
            application, wallpaper_backend
        );
        runtime->activate();
        runtime->inject_stress_notifications(150);
        runtime->toggle_right_sidebar();
        runtime->show_osd_volume();
        runtime->show_osd_brightness();
        drain_main_context(std::chrono::milliseconds(80));
        runtime->toggle_right_sidebar();
        drain_main_context(std::chrono::milliseconds(30));

        runtime.reset();
        // Drain callbacks that were queued immediately before destruction. Their
        // lifetime tokens must discard them without dereferencing dead owners.
        drain_main_context(std::chrono::milliseconds(30));
        g_object_unref(application);
    }

    std::filesystem::remove_all(config_root, filesystem_error);
    std::cout << "Realmheart lifetime stress passed " << iterations
              << " iterations\n";
    return 0;
}

} // namespace realmheart::ui
