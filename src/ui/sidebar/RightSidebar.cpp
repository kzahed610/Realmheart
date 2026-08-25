#include "ui/sidebar/RightSidebar.hpp"

#include "effects/shell/ShellEffectView.hpp"

#include "animation/character/CharacterCompositor.hpp"
#include "ui/sidebar/ConnectivityPanel.hpp"
#include "ui/sidebar/NightLightPanel.hpp"
#include "ui/sidebar/SidebarFrame.hpp"

#include "core/TaskExecutor.hpp"
#include "services/Audio.hpp"
#include "services/Bluetooth.hpp"
#include "services/Brightness.hpp"
#include "services/NightLight.hpp"
#include "services/PowerProfiles.hpp"
#include "services/Wifi.hpp"
#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"
#include "ui/components/NotificationWidget.hpp"
#include "ui/components/SliderWidget.hpp"
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace realmheart::ui::sidebar {
namespace {

constexpr double kSidebarHeightFraction = 0.90;
constexpr int kSidebarRightMargin = 2;

std::filesystem::path character_enabled_preference_path() {
    if (const char* config_home = std::getenv("XDG_CONFIG_HOME");
        config_home != nullptr && *config_home != '\0') {
        return std::filesystem::path(config_home) /
            "realmheart/features/sidebar-character.enabled";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) /
            ".config/realmheart/features/sidebar-character.enabled";
    }
    return std::filesystem::temp_directory_path() /
        "realmheart-sidebar-character.enabled";
}

bool load_character_enabled_preference() {
    std::ifstream input(character_enabled_preference_path());
    if (!input) return true;

    std::string value;
    input >> value;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (value == "0" || value == "false" || value == "off" ||
        value == "disabled" || value == "no") {
        return false;
    }
    if (value == "1" || value == "true" || value == "on" ||
        value == "enabled" || value == "yes") {
        return true;
    }

    std::cerr << "Ignoring invalid sidebar character preference: " << value << '\n';
    return true;
}

std::filesystem::path character_hair_mode_preference_path() {
    if (const char* config_home = std::getenv("XDG_CONFIG_HOME");
        config_home != nullptr && *config_home != '\0') {
        return std::filesystem::path(config_home) /
            "realmheart/features/sidebar-character.hair-mode";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) /
            ".config/realmheart/features/sidebar-character.hair-mode";
    }
    return std::filesystem::temp_directory_path() /
        "realmheart-sidebar-character.hair-mode";
}

realmheart::animation::character::CharacterHairMode
load_character_hair_mode_preference() {
    using realmheart::animation::character::CharacterHairMode;
    std::ifstream input(character_hair_mode_preference_path());
    if (!input) return CharacterHairMode::Mesh;

    std::string value;
    input >> value;
    if (const auto mode =
            realmheart::animation::character::parse_character_hair_mode(value)) {
        return *mode;
    }
    std::cerr << "Ignoring invalid sidebar character hair mode: " << value << '\n';
    return CharacterHairMode::Mesh;
}

bool persist_character_hair_mode_preference(
    realmheart::animation::character::CharacterHairMode mode
) {
    const auto path = character_hair_mode_preference_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        std::cerr << "Unable to create character hair-mode preference directory: "
                  << error.message() << '\n';
        return false;
    }

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << realmheart::animation::character::character_hair_mode_name(mode)
               << '\n';
        output.flush();
        if (!output) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool persist_character_enabled_preference(bool enabled) {
    const auto path = character_enabled_preference_path();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        std::cerr << "Unable to create Realmheart feature preference directory: "
                  << error.message() << '\n';
        return false;
    }

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            std::cerr << "Unable to write sidebar character preference: "
                      << temporary << '\n';
            return false;
        }
        output << (enabled ? "enabled\n" : "disabled\n");
        output.flush();
        if (!output) {
            std::cerr << "Unable to flush sidebar character preference: "
                      << temporary << '\n';
            return false;
        }
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::cerr << "Unable to publish sidebar character preference: "
                  << error.message() << '\n';
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

GtkWidget* themed_icon(const char* path, int pixels, const char* css_class = nullptr) {
    realmheart::ui::bar::widgets::ThemedSvgIcon icon(path, pixels);
    if (css_class != nullptr) icon.add_css_class(css_class);
    return icon.widget();
}

GtkWidget* left_label(const char* text, const char* css_class = nullptr) {
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    if (css_class != nullptr) gtk_widget_add_css_class(label, css_class);
    return label;
}

std::string uptime_text() {
    std::ifstream input("/proc/uptime");
    double uptime_seconds = 0.0;
    if (!(input >> uptime_seconds) || uptime_seconds < 0.0) return "Uptime unavailable";

    const auto total_minutes = static_cast<long long>(uptime_seconds / 60.0);
    const auto days = total_minutes / (24 * 60);
    const auto hours = (total_minutes / 60) % 24;
    const auto minutes = total_minutes % 60;

    std::ostringstream result;
    result << "Uptime  ";
    if (days > 0) result << days << "d ";
    if (hours > 0 || days > 0) result << hours << "h ";
    result << minutes << "m";
    return result.str();
}

std::string unquote_os_release_value(std::string value) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return value;
    }

    value = value.substr(1, value.size() - 2);
    std::string decoded;
    decoded.reserve(value.size());
    bool escaped = false;
    for (const char character : value) {
        if (escaped) {
            decoded.push_back(character);
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else {
            decoded.push_back(character);
        }
    }
    if (escaped) decoded.push_back('\\');
    return decoded;
}

std::optional<std::string> os_release_value(std::string_view key) {
    constexpr std::array<const char*, 2> paths{
        "/etc/os-release",
        "/usr/lib/os-release",
    };

    const std::string prefix = std::string(key) + "=";
    for (const char* path : paths) {
        std::ifstream input(path);
        if (!input) continue;

        std::string line;
        while (std::getline(input, line)) {
            if (!line.starts_with(prefix)) continue;
            return unquote_os_release_value(line.substr(prefix.size()));
        }
    }
    return std::nullopt;
}

std::string distribution_name() {
    if (auto name = os_release_value("NAME"); name && !name->empty()) {
        return *name;
    }
    if (auto pretty = os_release_value("PRETTY_NAME");
        pretty && !pretty->empty()) {
        return *pretty;
    }
    return "Linux";
}

bool is_self_or_descendant(GtkWidget* widget, GtkWidget* ancestor) {
    if (widget == nullptr || ancestor == nullptr) return false;
    for (GtkWidget* current = widget;
         current != nullptr;
         current = gtk_widget_get_parent(current)) {
        if (current == ancestor) return true;
    }
    return false;
}

} // namespace

class QuickControlTile {
public:
    QuickControlTile(
        const char* label,
        const char* icon_path,
        std::function<void()> activated,
        std::function<void()> settings_activated = nullptr
    ) : activated_(std::move(activated)),
        settings_activated_(std::move(settings_activated)) {
        button_ = gtk_button_new();
        gtk_widget_add_css_class(button_, "realmheart-quick-tile");
        gtk_widget_set_hexpand(button_, TRUE);
        gtk_widget_set_size_request(button_, -1, 64);

        GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_halign(content, GTK_ALIGN_FILL);
        gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(content), themed_icon(
            icon_path, 21, "realmheart-quick-tile-icon"
        ));

        GtkWidget* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(copy, TRUE);
        gtk_widget_set_halign(copy, GTK_ALIGN_FILL);
        GtkWidget* title = gtk_label_new(label);
        gtk_widget_add_css_class(title, "realmheart-quick-tile-title");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
        gtk_label_set_single_line_mode(GTK_LABEL(title), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(copy), title);
        status_ = gtk_label_new("Unavailable");
        gtk_widget_add_css_class(status_, "realmheart-quick-tile-status");
        gtk_label_set_xalign(GTK_LABEL(status_), 0.0F);
        gtk_label_set_single_line_mode(GTK_LABEL(status_), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(status_), 18);
        gtk_box_append(GTK_BOX(copy), status_);
        gtk_box_append(GTK_BOX(content), copy);
        gtk_button_set_child(GTK_BUTTON(button_), content);

        g_signal_connect(button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<QuickControlTile*>(data);
            self->show_click_feedback();
            if (self->activated_) self->activated_();
        }), this);

        // Right-click opens the tile's settings panel (wifi networks, bluetooth
        // devices, night light temperature). Keep Awake has no settings, so it
        // simply omits the callback and right-click is a no-op.
        GtkGesture* right_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(
            GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY
        );
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(right_click), GTK_PHASE_CAPTURE
        );
        g_signal_connect(right_click, "pressed", G_CALLBACK(+[](
            GtkGestureClick*, int, double, double, gpointer data
        ) {
            auto* self = static_cast<QuickControlTile*>(data);
            if (self->settings_activated_) self->settings_activated_();
        }), this);
        gtk_widget_add_controller(button_, GTK_EVENT_CONTROLLER(right_click));
    }

    ~QuickControlTile() {
        if (click_feedback_timeout_ != 0) {
            g_source_remove(click_feedback_timeout_);
            click_feedback_timeout_ = 0;
        }
    }

    GtkWidget* widget() const { return button_; }

    void set_state(std::string status, bool active, bool available = true) {
        gtk_label_set_text(GTK_LABEL(status_), status.c_str());
        gtk_widget_remove_css_class(button_, "active");
        gtk_widget_remove_css_class(button_, "unavailable");
        if (active) gtk_widget_add_css_class(button_, "active");
        if (!available) gtk_widget_add_css_class(button_, "unavailable");
        gtk_widget_set_sensitive(button_, available);
    }

private:
    void show_click_feedback() {
        if (click_feedback_timeout_ != 0) {
            g_source_remove(click_feedback_timeout_);
            click_feedback_timeout_ = 0;
        }

        gtk_widget_remove_css_class(button_, "click-confirmed");
        gtk_widget_add_css_class(button_, "click-confirmed");
        click_feedback_timeout_ = g_timeout_add_full(
            G_PRIORITY_DEFAULT,
            155,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<QuickControlTile*>(data);
                self->click_feedback_timeout_ = 0;
                if (self->button_ != nullptr) {
                    gtk_widget_remove_css_class(
                        self->button_, "click-confirmed"
                    );
                }
                return G_SOURCE_REMOVE;
            },
            this,
            nullptr
        );
    }

    GtkWidget* button_ = nullptr;
    GtkWidget* status_ = nullptr;
    std::function<void()> activated_;
    std::function<void()> settings_activated_;
    guint click_feedback_timeout_ = 0;
};

SidebarPlacement sidebar_placement_for(GtkWidget* widget) {
    SidebarPlacement placement;

    // Try GDK's monitor list first (the fast path).
    int monitor_width = 0;
    int monitor_height = 0;

    if (GdkMonitor* monitor = resolve_layer_surface_monitor(widget)) {
        GdkRectangle geometry{};
        gdk_monitor_get_geometry(monitor, &geometry);
        g_object_unref(monitor);
        monitor_width = geometry.width;
        monitor_height = geometry.height;
    }

    // Fallback: query Hyprland directly via hyprctl. GDK's monitor list can
    // become permanently invalid (G_IS_LIST_MODEL assertion failures) after
    // certain gtk4-layer-shell operations, so we can't rely on it alone.
    if (monitor_height <= 0) {
        GError* error = nullptr;
        gchar* stdout_buf = nullptr;
        gchar* stderr_buf = nullptr;
        gint exit_status = 0;
        if (g_spawn_command_line_sync(
                "hyprctl monitors -j",
                &stdout_buf,
                &stderr_buf,
                &exit_status,
                &error
            ) && error == nullptr && exit_status == 0 && stdout_buf != nullptr) {
            // Minimal JSON extraction: find the first {"width":W,"height":H pair.
            // We avoid a full JSON parser to keep this lightweight; hyprctl's
            // output is stable and always well-formed.
            const std::string json(stdout_buf);
            const auto w_pos = json.find("\"width\"");
            const auto h_pos = json.find("\"height\"");
            if (w_pos != std::string::npos && h_pos != std::string::npos) {
                // Parse width: skip to the colon, then the number
                auto parse_after = [](const std::string& s, std::size_t pos) -> int {
                    auto colon = s.find(':', pos);
                    if (colon == std::string::npos) return 0;
                    // Find the first digit
                    auto digit_start = s.find_first_of("0123456789", colon);
                    if (digit_start == std::string::npos) return 0;
                    char* end = nullptr;
                    long val = std::strtol(s.c_str() + digit_start, &end, 10);
                    return static_cast<int>(val);
                };
                monitor_width = parse_after(json, w_pos);
                monitor_height = parse_after(json, h_pos);
            }
        }
        if (error) g_error_free(error);
        if (stdout_buf) g_free(stdout_buf);
        if (stderr_buf) g_free(stderr_buf);
    }

    if (monitor_height <= 0) return placement;

    placement.height = std::max(
        static_cast<int>(std::lround(
            static_cast<double>(monitor_height) * kSidebarHeightFraction
        )),
        1
    );
    placement.top_margin = std::max((monitor_height - placement.height) / 2, 0);
    (void)monitor_width; // reserved for future horizontal centering
    return placement;
}

RightSidebar::RightSidebar(
    GtkApplication* app,
    services::NotificationHistory& notification_history,
    std::function<void(double)> show_volume_osd,
    std::function<void(double)> show_brightness_osd
) : app_(app),
    keep_awake_(std::make_shared<services::KeepAwake>()),
    notification_history_(notification_history),
    show_volume_osd_(std::move(show_volume_osd)),
    show_brightness_osd_(std::move(show_brightness_osd)) {
    character_enabled_ = load_character_enabled_preference();
    character_hair_mode_ = load_character_hair_mode_preference();
    async_ui_state_->owner = this;
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Right Sidebar");
    gtk_widget_add_css_class(window_, "realmheart-sidebar-window");
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);

    // Initial default size (will be updated on realize)
    gtk_window_set_default_size(
        GTK_WINDOW(window_),
        kDefaultSidebarFrameLayout.surface_width(),
        760
    );

    auto layer_spec = make_layer_surface_spec(
        "realmheart-right-sidebar",
        LayerSurfaceLevel::Overlay,
        LayerKeyboardMode::OnDemand
    );
    layer_spec.anchor_bottom = false;
    layer_spec.margin_top = 76; // default, updated on realize
    layer_spec.margin_right = kSidebarRightMargin;
    apply_layer_surface(GTK_WINDOW(window_), layer_spec);

    // Defer geometry computation until the window is realized and we can
    // query the monitor. resolve_layer_surface_monitor() returns nullptr
    // before realization.
    g_signal_connect(window_, "realize", G_CALLBACK(+[](GtkWidget* widget, gpointer data) {
        auto* self = static_cast<RightSidebar*>(data);
        self->update_geometry_on_realize(widget);
    }), this);

    setup_layout();
    populate_modules();
    install_panel_click_away();
    refresh_controls();
}

RightSidebar::~RightSidebar() {
    cancel_character_hide_timeout();
    cancel_prewarm();
    if (power_feedback_timeout_ != 0) {
        g_source_remove(power_feedback_timeout_);
        power_feedback_timeout_ = 0;
    }
    async_ui_state_->alive = false;
    async_ui_state_->owner = nullptr;
    wifi_panel_.reset();
    bluetooth_panel_.reset();
    night_light_panel_.reset();
    character_compositor_.reset();
    modules_.clear();
    wifi_tile_.reset();
    bluetooth_tile_.reset();
    night_light_tile_.reset();
    keep_awake_tile_.reset();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void RightSidebar::update_geometry_on_realize(GtkWidget* widget) {
    const auto placement = sidebar_placement_for(widget);
    gtk_window_set_default_size(
        GTK_WINDOW(widget),
        kDefaultSidebarFrameLayout.surface_width(),
        placement.height
    );

    // Update the layer surface margin for proper vertical centering
    gtk_layer_set_margin(GTK_WINDOW(widget), GTK_LAYER_SHELL_EDGE_TOP, placement.top_margin);
}

void RightSidebar::apply_geometry() {
    if (window_ == nullptr) return;
    const auto placement = sidebar_placement_for(window_);
    gtk_window_set_default_size(
        GTK_WINDOW(window_),
        kDefaultSidebarFrameLayout.surface_width(),
        placement.height
    );
    gtk_layer_set_margin(
        GTK_WINDOW(window_),
        GTK_LAYER_SHELL_EDGE_TOP,
        placement.top_margin
    );
}

void RightSidebar::setup_layout() {
    frame_ = std::make_unique<SidebarFrame>(
        GTK_WINDOW(window_), kDefaultSidebarFrameLayout
    );

    content_overlay_ = gtk_overlay_new();
    gtk_widget_add_css_class(content_overlay_, "realmheart-sidebar-content-overlay");
    gtk_widget_set_hexpand(content_overlay_, TRUE);
    gtk_widget_set_vexpand(content_overlay_, TRUE);

    container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(container_, "realmheart-right-sidebar");
    gtk_widget_set_hexpand(container_, TRUE);
    gtk_widget_set_vexpand(container_, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(content_overlay_), container_);

    frame_->set_child(content_overlay_);
    effect_view_ = realmheart_shell_effect_view_new(frame_->widget());
    gtk_widget_set_hexpand(effect_view_, TRUE);
    gtk_widget_set_vexpand(effect_view_, TRUE);
    effects::shell::set_origin(
        REALMHEART_SHELL_EFFECT_VIEW(effect_view_),
        1.0,
        0.5
    );
    gtk_window_set_child(GTK_WINDOW(window_), effect_view_);

    if (character_enabled_) initialize_character_compositor();
}

void RightSidebar::initialize_character_compositor() {
    if (character_compositor_ || frame_ == nullptr) return;

    int preferred_scale = 1;
    if (GdkMonitor* monitor = resolve_layer_surface_monitor(window_)) {
        preferred_scale = std::max(gdk_monitor_get_scale_factor(monitor), 1);
        g_object_unref(monitor);
    }

    const auto placement = sidebar_placement_for(window_);
    std::string character_error;
    const auto character_rig = resolve_project_asset("characters/tessia/rig.json");
    if (!character_rig) {
        std::cerr << "Unable to locate sidebar character rig\n";
        return;
    }

    character_compositor_ =
        realmheart::animation::character::CharacterCompositor::create(
            frame_->back_art_layer(),
            frame_->front_art_layer(),
            character_rig->parent_path(),
            preferred_scale,
            {
                .occlusion_left = static_cast<double>(
                    frame_->layout().frame_origin_x()
                ),
                .occlusion_top = 0.0,
                .surface_width = frame_->layout().surface_width(),
                .surface_height = placement.height,
            },
            &character_error
        );
    if (!character_compositor_) {
        std::cerr << "Unable to initialize sidebar character composition: "
                  << character_error << '\n';
    } else {
        std::string hair_mode_error;
        if (!character_compositor_->set_hair_mode(
                character_hair_mode_,
                &hair_mode_error
            )) {
            std::cerr << "Unable to initialize character hair mode "
                      << realmheart::animation::character::character_hair_mode_name(
                             character_hair_mode_
                         )
                      << ": " << hair_mode_error << '\n';
            character_hair_mode_ =
                realmheart::animation::character::CharacterHairMode::Mesh;
            static_cast<void>(character_compositor_->set_hair_mode(
                character_hair_mode_
            ));
            static_cast<void>(persist_character_hair_mode_preference(
                character_hair_mode_
            ));
        }
    }
}

void RightSidebar::cancel_character_hide_timeout() {
    if (character_hide_timeout_ != 0) {
        g_source_remove(character_hide_timeout_);
        character_hide_timeout_ = 0;
    }
    character_hide_completion_ = {};
}

gboolean RightSidebar::finish_character_hide(gpointer raw) {
    auto* self = static_cast<RightSidebar*>(raw);
    self->character_hide_timeout_ = 0;

    auto completion = std::move(self->character_hide_completion_);
    self->character_hide_completion_ = {};
    if (!self->sidebar_presented_ && completion) completion();
    return G_SOURCE_REMOVE;
}

void RightSidebar::prewarm() {
    if (prewarmed_) return;
    if (window_ == nullptr) return;
    prewarmed_ = true;

    // First-map pipeline warmup: realize geometry, then map once at opacity
    // 0 and let exactly one frame clock tick render through GSK/EGL before
    // unmapping. The panel is invisible throughout; OnDemand keyboard mode
    // means the map never grabs input.
    apply_geometry();
    gtk_widget_set_opacity(window_, 0.0);
    gtk_widget_set_visible(window_, TRUE);

    prewarm_frame_active_ = true;
    prewarm_tick_id_ = gtk_widget_add_tick_callback(
        window_,
        +[](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
            auto* self = static_cast<RightSidebar*>(data);
            self->prewarm_tick_id_ = 0;
            self->prewarm_frame_active_ = false;
            if (!self->sidebar_presented_) {
                gtk_widget_set_visible(self->window_, FALSE);
            }
            gtk_widget_set_opacity(self->window_, 1.0);
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

void RightSidebar::cancel_prewarm() {
    if (prewarm_tick_id_ != 0 && window_ != nullptr) {
        gtk_widget_remove_tick_callback(window_, prewarm_tick_id_);
        prewarm_tick_id_ = 0;
    }
    if (prewarm_frame_active_) {
        prewarm_frame_active_ = false;
        if (window_ != nullptr) gtk_widget_set_opacity(window_, 1.0);
    }
}

void RightSidebar::animate_character_in() {
    sidebar_presented_ = true;
    cancel_character_hide_timeout();
    if (!character_enabled_) return;

    initialize_character_compositor();
    if (character_compositor_) character_compositor_->start_enter();
}

void RightSidebar::set_surface_effect(
    effects::EffectId effect,
    double progress
) {
    if (effect_view_ == nullptr) return;
    effects::shell::set_frame(
        REALMHEART_SHELL_EFFECT_VIEW(effect_view_),
        effects::sample_effect(effect, progress)
    );
}

bool RightSidebar::animate_character_out(std::function<void()> completion) {
    sidebar_presented_ = false;
    cancel_character_hide_timeout();
    if (!character_enabled_ || !character_compositor_) return false;

    character_compositor_->start_exit();
    character_hide_completion_ = std::move(completion);
    character_hide_timeout_ = g_timeout_add(
        realmheart::animation::character::CharacterCompositor::exit_duration_ms() + 20,
        &RightSidebar::finish_character_hide,
        this
    );
    return true;
}

void RightSidebar::set_character_enabled(bool enabled) {
    if (character_enabled_ == enabled) return;

    character_enabled_ = enabled;
    if (character_enabled_) {
        initialize_character_compositor();
        if (sidebar_presented_ && character_compositor_) {
            character_compositor_->start_enter();
        }
    } else {
        // Actual shutdown rather than visibility: this removes the drawing
        // areas, the active tick callback, and all decoded textures.
        character_compositor_.reset();
#if defined(__GLIBC__)
        // The kill switch is an explicit resource-release path. Cairo surfaces
        // are gone at this point; ask glibc to return their now-free heap pages
        // instead of retaining the previous character high-water mark.
        static_cast<void>(malloc_trim(0));
#endif

        // If the kill switch lands during the short exit grace period, finish
        // the pending close immediately instead of leaving the window mapped.
        if (!sidebar_presented_ && character_hide_timeout_ != 0) {
            g_source_remove(character_hide_timeout_);
            character_hide_timeout_ = 0;
            auto completion = std::move(character_hide_completion_);
            character_hide_completion_ = {};
            if (completion) completion();
        }
    }

    static_cast<void>(persist_character_enabled_preference(character_enabled_));
    std::cerr << "Sidebar character "
              << (character_enabled_ ? "enabled" : "disabled") << '\n';
}

void RightSidebar::toggle_character() {
    set_character_enabled(!character_enabled_);
}

bool RightSidebar::apply_character_hair_mode(
    realmheart::animation::character::CharacterHairMode mode
) {
    if (character_enabled_) initialize_character_compositor();
    if (character_compositor_) {
        std::string hair_mode_error;
        if (!character_compositor_->set_hair_mode(mode, &hair_mode_error)) {
            std::cerr << "Unable to switch character hair mode to "
                      << realmheart::animation::character::character_hair_mode_name(mode)
                      << ": " << hair_mode_error << '\n';
            return false;
        }
    }

    character_hair_mode_ = mode;
    static_cast<void>(persist_character_hair_mode_preference(character_hair_mode_));
    std::cerr << "Character hair mode set to "
              << realmheart::animation::character::character_hair_mode_name(
                     character_hair_mode_
                 )
              << '\n';
    return true;
}

bool RightSidebar::set_character_hair_mode(std::string_view mode_name) {
    const auto parsed =
        realmheart::animation::character::parse_character_hair_mode(mode_name);
    if (!parsed) {
        std::cerr << "Unknown character hair mode: " << mode_name
                  << " (expected static, mesh, or mesh-flow)\n";
        return false;
    }
    return apply_character_hair_mode(*parsed);
}

bool RightSidebar::set_character_quality_preset(
    realmheart::animation::character::CharacterQualityPreset preset
) {
    const auto policy =
        realmheart::animation::character::character_quality_policy(preset);
    return apply_character_hair_mode(policy.hair_mode);
}


void RightSidebar::build_identity_header() {
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(header, "realmheart-identity-header");

    GtkWidget* title = gtk_label_new("REALMHEART");
    gtk_widget_add_css_class(title, "realmheart-identity-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5F);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(header), title);

    GtkWidget* state_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(state_row, "realmheart-identity-state-row");
    const std::string distro = distribution_name();
    GtkWidget* subtitle = left_label(
        distro.c_str(), "realmheart-identity-subtitle"
    );
    gtk_widget_set_hexpand(subtitle, TRUE);
    gtk_label_set_single_line_mode(GTK_LABEL(subtitle), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 26);
    gtk_box_append(GTK_BOX(state_row), subtitle);
    online_label_ = left_label("●  ONLINE", "realmheart-online-state");
    gtk_label_set_single_line_mode(GTK_LABEL(online_label_), TRUE);
    gtk_box_append(GTK_BOX(state_row), online_label_);
    uptime_label_ = left_label("Uptime", "realmheart-uptime");
    gtk_label_set_single_line_mode(GTK_LABEL(uptime_label_), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(uptime_label_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(uptime_label_), 18);
    gtk_box_append(GTK_BOX(state_row), uptime_label_);
    gtk_box_append(GTK_BOX(header), state_row);
    gtk_box_append(GTK_BOX(container_), header);
}

void RightSidebar::build_quick_controls() {
    GtkWidget* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(section, "realmheart-sidebar-section");
    GtkWidget* heading = left_label("QUICK CONTROLS", "realmheart-section-title");
    gtk_box_append(GTK_BOX(section), heading);

    GtkWidget* grid = gtk_grid_new();
    gtk_widget_add_css_class(grid, "realmheart-quick-grid");
    gtk_grid_set_column_spacing(GTK_GRID(grid), 7);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 7);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    wifi_tile_ = std::make_unique<QuickControlTile>(
        "Wi-Fi", "Realmheart-Icons/wifi.svg",
        [this] {
            // Left click: hard toggle the radio. post_control_action runs the
            // mutation on the task executor and refreshes tile state after.
            post_control_action([] {
                const auto state = services::Wifi::read();
                if (!state) return;
                static_cast<void>(services::Wifi::set_enabled(!state->enabled));
            });
        },
        [this] {
            if (bluetooth_panel_) bluetooth_panel_->hide();
            if (night_light_panel_) night_light_panel_->hide();
            if (wifi_panel_) wifi_panel_->toggle();
        }
    );
    bluetooth_tile_ = std::make_unique<QuickControlTile>(
        "Bluetooth", "Realmheart-Icons/bluetooth.svg",
        [this] {
            post_control_action([] {
                const auto state = services::Bluetooth::read();
                if (!state) return;
                static_cast<void>(services::Bluetooth::set_powered(!state->powered));
            });
        },
        [this] {
            if (wifi_panel_) wifi_panel_->hide();
            if (night_light_panel_) night_light_panel_->hide();
            if (bluetooth_panel_) bluetooth_panel_->toggle();
        }
    );
    night_light_tile_ = std::make_unique<QuickControlTile>(
        "Night Light", "Realmheart-Icons/night-light.svg",
        [this] {
            post_control_action([] {
                const auto state = services::NightLight::read();
                if (!state) return;
                static_cast<void>(services::NightLight::set_enabled(!state->enabled));
            });
        },
        [this] {
            if (wifi_panel_) wifi_panel_->hide();
            if (bluetooth_panel_) bluetooth_panel_->hide();
            if (night_light_panel_) night_light_panel_->toggle();
        }
    );
    keep_awake_tile_ = std::make_unique<QuickControlTile>(
        "Keep Awake", "Realmheart-Icons/keep-awake.svg",
        [this] {
            const bool enabled = !keep_awake_->active();
            post_control_action([keep_awake = keep_awake_, enabled] {
                static_cast<void>(keep_awake->set_enabled(enabled));
            });
        }
    );

    gtk_grid_attach(GTK_GRID(grid), wifi_tile_->widget(), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bluetooth_tile_->widget(), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), night_light_tile_->widget(), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), keep_awake_tile_->widget(), 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(section), grid);
    gtk_box_append(GTK_BOX(container_), section);

    wifi_panel_ = std::make_unique<WifiManagerPopover>(
        content_overlay_, [this] { refresh_controls(); }
    );
    bluetooth_panel_ = std::make_unique<BluetoothManagerPopover>(
        content_overlay_, [this] { refresh_controls(); }
    );
    night_light_panel_ = std::make_unique<NightLightPanel>(
        content_overlay_, [this] { refresh_controls(); }
    );
}

void RightSidebar::build_power_profiles() {
    GtkWidget* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(section, "realmheart-sidebar-section");
    gtk_widget_add_css_class(section, "realmheart-power-section");
    gtk_box_append(GTK_BOX(section), left_label(
        "POWER PROFILE", "realmheart-section-title"
    ));

    GtkWidget* segments = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(segments, "realmheart-power-segments");
    constexpr std::array<const char*, 3> profiles{
        "power-saver", "balanced", "performance"
    };
    constexpr std::array<const char*, 3> labels{
        "Power", "Balanced", "Performance"
    };
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        GtkWidget* button = gtk_button_new_with_label(labels[index]);
        gtk_widget_add_css_class(button, "realmheart-power-segment");
        gtk_widget_set_hexpand(button, TRUE);
        g_object_set_data_full(
            G_OBJECT(button), "realmheart-profile",
            g_strdup(profiles[index]), g_free
        );
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* pressed, gpointer data) {
            auto* self = static_cast<RightSidebar*>(data);
            const char* profile = static_cast<const char*>(g_object_get_data(
                G_OBJECT(pressed), "realmheart-profile"
            ));
            if (profile != nullptr) self->set_power_profile(profile);
        }), this);
        power_profile_buttons_[index] = button;
        gtk_box_append(GTK_BOX(segments), button);
    }
    gtk_box_append(GTK_BOX(section), segments);
    gtk_box_append(GTK_BOX(container_), section);
}

void RightSidebar::install_panel_click_away() {
    GtkGesture* click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE
    );
    g_signal_connect(click, "pressed", G_CALLBACK(+[](
        GtkGestureClick*, int, double x, double y, gpointer data
    ) {
        auto* self = static_cast<RightSidebar*>(data);
        if (self->frame_ == nullptr) return;

        GtkWidget* frame = self->frame_->widget();
        GtkWidget* picked = gtk_widget_pick(frame, x, y, GTK_PICK_DEFAULT);
        if (picked == nullptr) return;

        // Let clicks within the currently materialised panel pass through.
        // The reveal clip's contains() implementation already excludes the
        // still-hidden part of a panel while it is opening or closing.
        if ((self->wifi_panel_ != nullptr && is_self_or_descendant(
                 picked, self->wifi_panel_->widget()
             )) ||
            (self->bluetooth_panel_ != nullptr && is_self_or_descendant(
                 picked, self->bluetooth_panel_->widget()
             )) ||
            (self->night_light_panel_ != nullptr && is_self_or_descendant(
                 picked, self->night_light_panel_->widget()
             ))) {
            return;
        }

        // These three launchers own their own toggle/switch behaviour. Hiding
        // during capture would make clicking the active launcher close and
        // immediately reopen its panel when the button's clicked signal runs.
        if ((self->wifi_tile_ != nullptr && is_self_or_descendant(
                 picked, self->wifi_tile_->widget()
             )) ||
            (self->bluetooth_tile_ != nullptr && is_self_or_descendant(
                 picked, self->bluetooth_tile_->widget()
             )) ||
            (self->night_light_tile_ != nullptr && is_self_or_descendant(
                 picked, self->night_light_tile_->widget()
             ))) {
            return;
        }

        if (self->wifi_panel_ != nullptr) self->wifi_panel_->hide();
        if (self->bluetooth_panel_ != nullptr) self->bluetooth_panel_->hide();
        if (self->night_light_panel_ != nullptr) self->night_light_panel_->hide();
    }), this);
    gtk_widget_add_controller(frame_->widget(), GTK_EVENT_CONTROLLER(click));
}

void RightSidebar::populate_modules() {
    build_identity_header();
    build_quick_controls();

    GtkWidget* slider_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(slider_section, "realmheart-sidebar-section");
    gtk_widget_add_css_class(slider_section, "realmheart-levels-section");
    gtk_box_append(GTK_BOX(slider_section), left_label(
        "SYSTEM LEVELS", "realmheart-section-title"
    ));

    GtkWidget* sliders = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sliders, "realmheart-slider-chamber");

    auto brightness_widget = std::make_unique<components::SliderWidget>(
        "Brightness",
        0,
        100,
        0.0,
        [](double value) {
            const auto mutation = services::Brightness::set_percent(
                static_cast<int>(std::lround(value))
            );
            if (!mutation.success) return std::optional<double>{};
            return std::optional<double>{mutation.state.percent};
        },
        show_brightness_osd_
    );
    brightness_slider_ = brightness_widget.get();
    brightness_slider_->set_available(false);
    gtk_box_append(GTK_BOX(sliders), brightness_widget->get_widget());
    modules_.push_back(std::move(brightness_widget));

    auto volume_widget = std::make_unique<components::SliderWidget>(
        "Volume",
        0,
        100,
        0.0,
        [](double value) {
            const auto mutation = services::Audio::set_default_sink_volume(value / 100.0);
            if (!mutation.success) return std::optional<double>{};
            return std::optional<double>{mutation.state.volume * 100.0};
        },
        show_volume_osd_
    );
    volume_slider_ = volume_widget.get();
    volume_slider_->set_available(false);
    gtk_box_append(GTK_BOX(sliders), volume_widget->get_widget());
    modules_.push_back(std::move(volume_widget));

    gtk_box_append(GTK_BOX(slider_section), sliders);
    gtk_box_append(GTK_BOX(container_), slider_section);

    build_power_profiles();

    auto notifications = std::make_unique<components::NotificationWidget>(
        notification_history_
    );
    GtkWidget* notification_widget = notifications->get_widget();
    // Notifications absorb the spare vertical space below the power section.
    gtk_widget_set_vexpand(notification_widget, TRUE);
    gtk_box_append(GTK_BOX(container_), notification_widget);
    modules_.push_back(std::move(notifications));
}

void RightSidebar::refresh() {
    refresh_controls();
}

void RightSidebar::refresh_controls() {
    // Keep the open path GTK-only and cheap. NotificationWidget already tracks
    // history changes through its subscription, so rebuilding every row here
    // would only delay presentation of the sidebar.
    const bool online = g_network_monitor_get_network_available(
        g_network_monitor_get_default()
    );
    gtk_label_set_text(GTK_LABEL(online_label_), online ? "●  ONLINE" : "●  OFFLINE");
    gtk_widget_remove_css_class(online_label_, "offline");
    if (!online) gtk_widget_add_css_class(online_label_, "offline");
    gtk_label_set_text(GTK_LABEL(uptime_label_), uptime_text().c_str());
    const bool keep_awake_active = keep_awake_->active();
    keep_awake_tile_->set_state(
        keep_awake_active ? "Enabled" : "Disabled",
        keep_awake_active,
        true
    );

    const auto state = async_ui_state_;
    if (state->refresh_in_flight.exchange(true)) {
        state->refresh_pending = true;
        return;
    }

    const std::uint64_t brightness_generation = brightness_slider_ != nullptr
        ? brightness_slider_->refresh_generation()
        : 0;
    const std::uint64_t volume_generation = volume_slider_ != nullptr
        ? volume_slider_->refresh_generation()
        : 0;

    const bool queued = realmheart::core::shared_task_executor().post([
        state, brightness_generation, volume_generation
    ] {
        std::optional<services::WifiState> wifi;
        std::optional<services::BluetoothState> bluetooth;
        std::optional<services::NightLightState> night_light;
        std::optional<std::string> active_profile;
        std::optional<services::BrightnessState> brightness;
        std::optional<services::AudioState> audio;
        {
            // Serialize sidebar-level service operations while keeping every
            // subprocess off GTK's main thread. Slider generations below reject
            // any read that became stale during a user mutation.
            std::lock_guard lock(state->operation_mutex);
            if (!state->alive.load()) {
                state->refresh_in_flight = false;
                return;
            }
            wifi = services::Wifi::read();
            bluetooth = services::Bluetooth::read();
            night_light = services::NightLight::read();
            active_profile = services::PowerProfiles::current();
            brightness = services::Brightness::read();
            audio = services::Audio::read_default_sink();
        }

        auto* result = new ControlRefreshResult{
            .state = state,
            .wifi_status = std::nullopt,
            .wifi_active = false,
            .bluetooth_powered = bluetooth
                ? std::optional<bool>{bluetooth->powered}
                : std::nullopt,
            .night_light_enabled = night_light
                ? std::optional<bool>{night_light->enabled}
                : std::nullopt,
            .active_profile = std::move(active_profile),
            .brightness_percent = brightness
                ? std::optional<double>{brightness->percent}
                : std::nullopt,
            .volume_percent = audio
                ? std::optional<double>{std::clamp(audio->volume * 100.0, 0.0, 100.0)}
                : std::nullopt,
            .brightness_generation = brightness_generation,
            .volume_generation = volume_generation,
        };
        if (wifi) {
            result->wifi_active = wifi->enabled;
            result->wifi_status = !wifi->enabled
                ? "Off"
                : (wifi->ssid.empty() ? "Disconnected" : wifi->ssid);
        }

        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            &RightSidebar::finish_control_refresh,
            result,
            &RightSidebar::destroy_control_refresh_result
        );
    });
    if (!queued) state->refresh_in_flight = false;
}

gboolean RightSidebar::finish_control_refresh(gpointer raw) {
    auto* result = static_cast<ControlRefreshResult*>(raw);
    auto& state = *result->state;
    auto* owner = state.owner;
    if (state.alive.load() && owner != nullptr) {
        if (result->wifi_status) {
            owner->wifi_tile_->set_state(
                *result->wifi_status, result->wifi_active, true
            );
        } else {
            owner->wifi_tile_->set_state("Unavailable", false, false);
        }

        if (result->bluetooth_powered) {
            owner->bluetooth_tile_->set_state(
                *result->bluetooth_powered ? "On" : "Off",
                *result->bluetooth_powered,
                true
            );
        } else {
            owner->bluetooth_tile_->set_state("Unavailable", false, false);
        }

        if (result->night_light_enabled) {
            owner->night_light_tile_->set_state(
                *result->night_light_enabled ? "On" : "Off",
                *result->night_light_enabled,
                true
            );
        } else {
            owner->night_light_tile_->set_state("Unavailable", false, false);
        }

        if (owner->brightness_slider_ != nullptr) {
            owner->brightness_slider_->apply_refresh(
                result->brightness_percent,
                result->brightness_generation
            );
        }

        if (owner->volume_slider_ != nullptr) {
            owner->volume_slider_->apply_refresh(
                result->volume_percent,
                result->volume_generation
            );
        }

        GtkWidget* pending_button = nullptr;
        bool pending_succeeded = false;
        for (std::size_t index = 0;
             index < owner->power_profile_buttons_.size();
             ++index) {
            GtkWidget* button = owner->power_profile_buttons_[index];
            gtk_widget_set_sensitive(button, result->active_profile.has_value());
            gtk_widget_remove_css_class(button, "active");
            gtk_widget_remove_css_class(button, "pending");
            const char* profile = static_cast<const char*>(
                g_object_get_data(G_OBJECT(button), "realmheart-profile")
            );
            if (result->active_profile && profile != nullptr &&
                *result->active_profile == profile) {
                gtk_widget_add_css_class(button, "active");
            }
            if (!owner->pending_power_profile_.empty() && profile != nullptr &&
                owner->pending_power_profile_ == profile) {
                pending_button = button;
                pending_succeeded = result->active_profile &&
                    *result->active_profile == profile;
            }
        }

        if (!owner->pending_power_profile_.empty()) {
            owner->show_power_profile_feedback(
                pending_button,
                pending_succeeded
            );
            owner->pending_power_profile_.clear();
        }
    }

    state.refresh_in_flight = false;
    if (state.alive.load() && owner != nullptr &&
        state.refresh_pending.exchange(false)) {
        owner->refresh_controls();
    }
    return G_SOURCE_REMOVE;
}

void RightSidebar::destroy_control_refresh_result(gpointer raw) {
    delete static_cast<ControlRefreshResult*>(raw);
}

void RightSidebar::post_control_action(std::function<void()> action) {
    const auto state = async_ui_state_;
    const auto generation = state->generation.fetch_add(1) + 1;
    realmheart::core::shared_task_executor().post([
        state, generation, action = std::move(action)
    ] {
        {
            std::lock_guard lock(state->operation_mutex);
            if (!state->alive.load() || state->generation.load() != generation) return;
            if (action) action();
        }
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<std::pair<std::shared_ptr<AsyncUiState>, std::uint64_t>*>(raw);
                if (payload->first->alive.load() && payload->first->owner != nullptr &&
                    payload->first->generation.load() == payload->second) {
                    payload->first->owner->refresh_controls();
                }
                return G_SOURCE_REMOVE;
            },
            new std::pair<std::shared_ptr<AsyncUiState>, std::uint64_t>{state, generation},
            +[](gpointer raw) {
                delete static_cast<std::pair<std::shared_ptr<AsyncUiState>, std::uint64_t>*>(raw);
            }
        );
    });
}

void RightSidebar::clear_power_profile_feedback() {
    if (power_feedback_timeout_ != 0) {
        g_source_remove(power_feedback_timeout_);
        power_feedback_timeout_ = 0;
    }
    for (GtkWidget* button : power_profile_buttons_) {
        gtk_widget_remove_css_class(button, "confirmed");
        gtk_widget_remove_css_class(button, "failed");
    }
}

void RightSidebar::show_power_profile_feedback(
    GtkWidget* button,
    bool success
) {
    clear_power_profile_feedback();
    if (button == nullptr) return;

    gtk_widget_add_css_class(button, success ? "confirmed" : "failed");
    power_feedback_timeout_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        success ? 320U : 460U,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<RightSidebar*>(data);
            self->power_feedback_timeout_ = 0;
            for (GtkWidget* candidate : self->power_profile_buttons_) {
                gtk_widget_remove_css_class(candidate, "confirmed");
                gtk_widget_remove_css_class(candidate, "failed");
            }
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

void RightSidebar::set_power_profile(const std::string& profile) {
    clear_power_profile_feedback();
    pending_power_profile_ = profile;
    for (GtkWidget* button : power_profile_buttons_) {
        gtk_widget_remove_css_class(button, "active");
        gtk_widget_remove_css_class(button, "pending");
        const char* candidate = static_cast<const char*>(
            g_object_get_data(G_OBJECT(button), "realmheart-profile")
        );
        if (candidate != nullptr && profile == candidate) {
            gtk_widget_add_css_class(button, "active");
            gtk_widget_add_css_class(button, "pending");
        }
        gtk_widget_set_sensitive(button, FALSE);
    }

    post_control_action([profile] {
        static_cast<void>(services::PowerProfiles::set(profile));
    });
}

} // namespace realmheart::ui::sidebar
