#include "ui/ShellApp.hpp"

#include "core/ShellControl.hpp"
#include "core/TaskExecutor.hpp"
#include "services/Audio.hpp"
#include "services/AudioMonitor.hpp"
#include "services/BatteryService.hpp"
#include "services/Brightness.hpp"
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
#include "ui/NotesOverlay.hpp"
#include "ui/NotificationToast.hpp"
#include "ui/NowPlayingOverlay.hpp"
#include "ui/OSDOverlay.hpp"
#include "ui/ShellState.hpp"
#include "ui/ThemeStyles.hpp"
#include "ui/bar/VerticalBar.hpp"
#include "ui/launcher/LauncherOverlay.hpp"
#include "ui/sidebar/RightSidebar.hpp"
#include "ui/sidebar/SidebarFrame.hpp"
#include "ui/wallpaper/WallpaperBackend.hpp"
#include "ui/wallpaper/WallpaperController.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
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

constexpr int kHotspotHitWidth = 16;

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
constexpr int kSidebarRightMargin = 2;

struct HotspotInputSetup {
    int frames_remaining = kHotspotInputCommitFrames;
};

struct BackdropInputSetup {
    int frames_remaining = kHotspotInputCommitFrames;
    int sidebar_top_margin = 0;
    int sidebar_height = 1;
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
    const int sidebar_width = sidebar::kDefaultSidebarFrameLayout.surface_width();
    const int sidebar_x = std::max(width - kSidebarRightMargin - sidebar_width, 0);
    const int sidebar_y = std::clamp(setup.sidebar_top_margin, 0, height);
    const int sidebar_region_width = std::clamp(
        sidebar_width,
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
        const int edge_x = std::max(width - kHotspotHitWidth, 0);
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
        " edge_width=", kHotspotHitWidth
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
    struct NowPlayingAsyncState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> generation{0};
        std::atomic<bool> refresh_in_flight{false};
        std::atomic<bool> refresh_pending{false};
        std::atomic<ShellRuntime*> owner{nullptr};
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
          utilities_(std::make_unique<services::UtilityManager>(theme_service_)),
          session_(std::make_unique<services::SessionManager>()),
          battery_(std::make_unique<services::BatteryService>()),
          media_(std::make_shared<services::MediaService>()),
          notes_service_(std::make_unique<services::NotesService>()),
          launcher_service_(std::make_unique<services::LauncherService>()) {
        now_playing_async_state_->owner.store(this);

        notification_server_.set_notification_handler([this](const auto& entry) {
            if (toast_) toast_->show(entry, 4000);
        });

        if (!notification_daemon_.start()) {
            std::cerr << "Unable to start Realmheart notification daemon\n";
        }
    }

    ~ShellRuntime() {
        // Stop callbacks that capture this before tearing down UI/controllers.
        now_playing_async_state_->alive.store(false);
        now_playing_async_state_->owner.store(nullptr);
        now_playing_subscription_.reset();
        notification_server_.set_notification_handler({});
        notification_daemon_.stop();
        audio_monitor_.reset();

        wallpaper_controller_.reset();
        launcher_overlay_.reset();
        notes_overlay_.reset();
        toast_.reset();
        osd_.reset();
        now_playing_.reset();

        sidebar_.reset();
        bar_.reset();

        if (sidebar_backdrop_ != nullptr) {
            gtk_window_destroy(sidebar_backdrop_);
            sidebar_backdrop_ = nullptr;
        }

        if (hotspot_ != nullptr) {
            gtk_window_destroy(hotspot_);
            hotspot_ = nullptr;
        }

        // Unsubscribe/remove the display-wide CSS provider while GTK is alive.
        theme_styles_.reset();
    }

    void activate() {
        ensure_initialized();
        state_.show_bar();
        apply_bar_visibility();

        const std::string current_path = utilities_->load_wallpaper_path();
        if (current_path.empty()) return;

        std::string error_message;
        if (!wallpaper_controller_->set_wallpaper(current_path, &error_message)) {
            std::cerr << "Unable to restore wallpaper: " << error_message << '\n';
            return;
        }
        generate_theme_for(current_path);
    }

    void toggle_right_sidebar() {
        ensure_initialized();
        const bool before = state_.right_sidebar_visible();
        state_.toggle_right_sidebar();
        sidebar_input_debug(
            "toggle_right_sidebar: ",
            before ? "open" : "closed",
            " -> ",
            state_.right_sidebar_visible() ? "open" : "closed"
        );
        apply_right_sidebar_visibility();
    }

    void show_osd_volume_value(double value) {
        ensure_initialized();
        osd_->show_volume(value);
    }

    void show_osd_brightness_value(double value) {
        ensure_initialized();
        osd_->show_brightness(value);
    }

    void show_osd_volume() {
        ensure_initialized();
        if (const auto audio = services::Audio::read_default_sink()) {
            double volume = audio->volume;
            if (volume > 1.0) {
                const auto normalized = services::Audio::set_default_sink_volume(1.0);
                volume = normalized.success ? normalized.state.volume : 1.0;
            }
            show_osd_volume_value(std::clamp(volume * 100.0, 0.0, 100.0));
        }
    }

    void show_osd_brightness() {
        ensure_initialized();
        if (const auto brightness = services::Brightness::read()) {
            show_osd_brightness_value(brightness->percent);
        }
    }

    void toggle_bar() {
        ensure_initialized();
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
        ensure_initialized();
        launcher_overlay_->toggle();
    }

    void set_wallpaper(const std::string& path = {}) {
        ensure_initialized();
        if (path.empty()) {
            choose_wallpaper_native();
            return;
        }

        std::string error_message;
        if (!wallpaper_controller_->set_wallpaper(path, &error_message)) {
            std::cerr << "Unable to set wallpaper: " << error_message << '\n';
            return;
        }

        if (services::WallpaperService* service = utilities_->get_wallpaper_service()) {
            if (!service->update_state(path)) {
                std::cerr << "Wallpaper changed, but its path could not be persisted\n";
            }
        }

        // Generate from the exact image that was just accepted by the renderer.
        // Previously this was only called at startup, leaving the fallback palette
        // unchanged for every later wallpaper swap.
        generate_theme_for(path);
    }

    void switch_wallpaper_backend(const std::string& backend_name) {
        ensure_initialized();
        const auto backend = wallpaper::parse_wallpaper_backend_type(backend_name);
        if (!backend) {
            std::cerr << "Unknown wallpaper backend: " << backend_name
                      << " (expected gtk or native)\n";
            return;
        }

        std::string error_message;
        if (!wallpaper_controller_->switch_backend(*backend, &error_message)) {
            std::cerr << "Unable to switch wallpaper backend to "
                      << backend_name << ": " << error_message << '\n';
            return;
        }

        requested_wallpaper_backend_ = *backend;
        std::cerr << "Wallpaper backend switched to " << backend_name << '\n';
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
        ensure_initialized();
        notes_overlay_->toggle();
    }

    void lock_session() {
        session_->lock();
    }

    void open_logout_menu() {
        session_->logout_menu();
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
        if (!info || !now_playing_) return;
        const std::string title = info->title.empty()
            ? "Unknown track"
            : info->title;
        const std::string artist = info->artist.empty() ? info->album : info->artist;
        now_playing_->show(title, artist);
    }

    void generate_theme_for(const std::string& path) {
        if (path.empty()) return;
        if (!utilities_->generate_colors(path)) {
            std::cerr << "[Theme] Keeping the current palette because generation failed\n";
        }
    }

    void ensure_initialized() {
        if (!theme_styles_) {
            theme_styles_ = std::make_unique<ThemeStyles>(theme_service_);
        }
        if (!toast_) toast_ = std::make_unique<NotificationToast>(application_);
        if (!now_playing_) {
            now_playing_ = std::make_unique<NowPlayingOverlay>(application_);
        }
        if (!osd_) {
            osd_ = std::make_unique<OSDOverlay>(
                application_,
                [this](bool visible) {
                    if (now_playing_) now_playing_->set_system_osd_visible(visible);
                }
            );
        }
        if (!audio_monitor_) {
            audio_monitor_ = std::make_unique<services::AudioMonitor>(
                [this](const services::AudioState& audio) {
                    const double percent = audio.muted
                        ? 0.0
                        : std::clamp(audio.volume * 100.0, 0.0, 100.0);
                    show_osd_volume_value(percent);
                }
            );
            audio_monitor_->start();
        }
        start_now_playing_monitor();
        if (!notes_overlay_) {
            notes_overlay_ = std::make_unique<NotesOverlay>(application_, notes_service_.get());
        }
        if (!bar_) {
            bar_ = std::make_unique<bar::VerticalBar>(
                application_,
                notification_history_,
                *battery_,
                *media_,
                [this] { toggle_right_sidebar(); },
                [this] { launch_launcher(); }
            );
        }
        if (!sidebar_) {
            sidebar_ = std::make_unique<sidebar::RightSidebar>(
                application_,
                notification_history_,
                [this](double value) { show_osd_volume_value(value); },
                [this](double value) { show_osd_brightness_value(value); }
            );
            gtk_widget_set_visible(sidebar_->get_window(), FALSE);
        }
        if (hotspot_ == nullptr) {
            hotspot_ = GTK_WINDOW(gtk_application_window_new(application_));
            gtk_window_set_decorated(hotspot_, FALSE);
            gtk_window_set_resizable(hotspot_, FALSE);
            gtk_widget_add_css_class(
                GTK_WIDGET(hotspot_),
                "realmheart-right-hotspot-window"
            );

            const auto placement = sidebar::sidebar_placement_for(
                GTK_WIDGET(hotspot_)
            );
            gtk_window_set_default_size(
                hotspot_,
                kHotspotHitWidth,
                placement.height
            );

            LayerSurfaceSpec spec;
            spec.surface_namespace = "realmheart-right-hotspot";
            spec.layer = LayerSurfaceLevel::Overlay;
            spec.anchor_right = true;
            spec.anchor_top = true;
            spec.anchor_bottom = false;
            spec.margin_top = placement.top_margin;
            apply_layer_surface(hotspot_, spec);

            GtkWidget* button = gtk_button_new();
            gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
            gtk_widget_set_focusable(button, FALSE);
            gtk_widget_set_hexpand(button, TRUE);
            gtk_widget_set_vexpand(button, TRUE);
            gtk_widget_set_size_request(
                button,
                kHotspotHitWidth,
                placement.height
            );
            gtk_widget_add_css_class(button, "realmheart-right-hotspot-button");
            g_signal_connect(
                button,
                "clicked",
                G_CALLBACK(+[](GtkButton*, gpointer data) {
                    sidebar_input_debug("hotspot button: clicked");
                    static_cast<ShellRuntime*>(data)->toggle_right_sidebar();
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

            gtk_window_set_child(hotspot_, overlay);
            gtk_window_present(hotspot_);
            enforce_hotspot_input_region(hotspot_);
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
                    static_cast<ShellRuntime*>(data)->toggle_right_sidebar();
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
        if (!launcher_overlay_) {
            launcher_overlay_ = std::make_unique<LauncherOverlay>(
                application_,
                *launcher_service_
            );
        }
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

    void apply_right_sidebar_visibility() {
        GtkWidget* window = sidebar_->get_window();
        if (state_.right_sidebar_visible()) {
            sidebar_input_debug("visibility: presenting backdrop then sidebar");
            // Use an Overlay backdrop with a carved input region instead of
            // relying on cross-layer stacking. Only clicks outside the sidebar
            // are accepted by this surface.
            gtk_window_present(sidebar_backdrop_);
            const auto placement = sidebar::sidebar_placement_for(
                GTK_WIDGET(sidebar_backdrop_)
            );
            enforce_sidebar_backdrop_input_region(
                sidebar_backdrop_,
                placement
            );

            sidebar_->refresh();
            gtk_window_present(GTK_WINDOW(window));
        } else {
            sidebar_input_debug("visibility: hiding sidebar and backdrop");
            // Hide, don't destroy: the controller and its workers remain valid.
            gtk_widget_set_visible(window, FALSE);
            gtk_widget_set_visible(GTK_WIDGET(sidebar_backdrop_), FALSE);
        }
    }

    void apply_bar_visibility() {
        GtkWidget* window = bar_->get_window();
        if (state_.bar_visible()) {
            bar_->refresh();
            gtk_window_present(GTK_WINDOW(window));
        } else {
            gtk_widget_set_visible(window, FALSE);
        }
    }

    GtkApplication* application_ = nullptr;
    wallpaper::WallpaperBackendType requested_wallpaper_backend_ =
        wallpaper::WallpaperBackendType::Gtk;

    services::NotificationHistory notification_history_;
    services::NotificationServer notification_server_;
    services::NotificationDaemon notification_daemon_;

    std::shared_ptr<services::ThemeService> theme_service_;
    std::unique_ptr<services::UtilityManager> utilities_;
    std::unique_ptr<services::SessionManager> session_;
    std::unique_ptr<services::BatteryService> battery_;
    std::shared_ptr<services::MediaService> media_;
    services::MediaService::Subscription now_playing_subscription_;
    std::shared_ptr<NowPlayingAsyncState> now_playing_async_state_ =
        std::make_shared<NowPlayingAsyncState>();
    bool now_playing_monitor_started_ = false;
    bool now_playing_seeded_ = false;
    std::string last_now_playing_identity_;
    std::unique_ptr<services::NotesService> notes_service_;
    std::unique_ptr<services::LauncherService> launcher_service_;

    std::unique_ptr<ThemeStyles> theme_styles_;
    std::unique_ptr<NotesOverlay> notes_overlay_;
    std::unique_ptr<NotificationToast> toast_;
    std::unique_ptr<NowPlayingOverlay> now_playing_;
    std::unique_ptr<OSDOverlay> osd_;
    std::unique_ptr<services::AudioMonitor> audio_monitor_;
    std::unique_ptr<bar::VerticalBar> bar_;
    GtkWindow* hotspot_ = nullptr;
    GtkWindow* sidebar_backdrop_ = nullptr;
    std::unique_ptr<sidebar::RightSidebar> sidebar_;
    std::unique_ptr<LauncherOverlay> launcher_overlay_;
    std::unique_ptr<wallpaper::WallpaperController> wallpaper_controller_;

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
    {"quit", quit_action, nullptr, nullptr, nullptr, {}},
};

} // namespace

int run_shell(wallpaper::WallpaperBackendType wallpaper_backend) {
    GtkApplication* application = gtk_application_new(
        realmheart::core::shell_application_id().data(),
        G_APPLICATION_DEFAULT_FLAGS
    );
    auto runtime = std::make_unique<ShellRuntime>(application, wallpaper_backend);

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

} // namespace realmheart::ui
