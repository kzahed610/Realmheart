#include "ui/powermenu/PowerMenuOverlay.hpp"

#include "ui/LayerSurface.hpp"
#include "ui/powermenu/PowerMenuControls.hpp"
#include "ui/powermenu/PowerMenuScene.hpp"

#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <iostream>
#include <string>
#include <utility>

namespace realmheart::ui::powermenu {
namespace {

constexpr guint kConfirmationTimeoutMs = 1200;
constexpr int kInputRegionCommitFrames = 30;

const char* action_name(PowerMenuOverlay::Action action) {
    switch (action) {
        case PowerMenuOverlay::Action::Lock: return "lock";
        case PowerMenuOverlay::Action::Suspend: return "suspend";
        case PowerMenuOverlay::Action::Logout: return "log out";
        case PowerMenuOverlay::Action::Reboot: return "restart";
        case PowerMenuOverlay::Action::PowerOff: return "shut down";
    }
    return "unknown";
}

const char* action_label(PowerMenuOverlay::Action action) {
    switch (action) {
        case PowerMenuOverlay::Action::Lock: return "LOCK";
        case PowerMenuOverlay::Action::Suspend: return "SUSPEND";
        case PowerMenuOverlay::Action::Logout: return "LOG OUT";
        case PowerMenuOverlay::Action::Reboot: return "RESTART";
        case PowerMenuOverlay::Action::PowerOff: return "SHUT DOWN";
    }
    return "UNKNOWN ACTION";
}

bool preview_mode_enabled() {
    const char* value = g_getenv("REALMHEART_POWER_MENU_PREVIEW");
    return value != nullptr && *value != '\0' && g_strcmp0(value, "0") != 0;
}

bool apply_full_input_region(GtkWidget* widget) {
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

    // Wayland applies the input region on a surface commit. Keep requesting a
    // render for several frames so a cold, otherwise-static overlay cannot map
    // with the empty input region produced before its controls are allocated.
    gdk_surface_queue_render(surface);
    return true;
}

} // namespace

PowerMenuOverlay::PowerMenuOverlay(GtkApplication* app, PowerMenuActions actions)
    : actions_(std::move(actions)) {
    window_ = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(window_, "Realmheart Power Menu");
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-power-menu";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::Exclusive;
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    apply_layer_surface(window_, spec);
    gtk_layer_set_exclusive_zone(window_, -1);

    GtkWidget* root = gtk_overlay_new();
    gtk_widget_set_hexpand(root, TRUE);
    gtk_widget_set_vexpand(root, TRUE);

    scene_ = std::make_unique<PowerMenuScene>();
    gtk_overlay_set_child(GTK_OVERLAY(root), scene_->widget());

    controls_ = std::make_unique<PowerMenuControls>(
        [this](Action action) { activate(action); },
        [this]() { hide(); }
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(root), controls_->widget());

    confirmation_banner_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(confirmation_banner_, "realmheart-power-confirmation");
    gtk_widget_set_halign(confirmation_banner_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(confirmation_banner_, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(confirmation_banner_, 36);
    gtk_widget_set_can_target(confirmation_banner_, FALSE);
    gtk_widget_set_visible(confirmation_banner_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root), confirmation_banner_);

    if (GdkDisplay* display = gdk_display_get_default(); display != nullptr) {
        GtkCssProvider* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, R"CSS(
            .realmheart-power-confirmation {
                background: rgba(17, 10, 28, 0.96);
                border: 2px solid #f2ce78;
                border-radius: 14px;
                box-shadow: 0 8px 28px rgba(0, 0, 0, 0.65);
                color: #f8e7b0;
                font-family: "Noto Serif Display", serif;
                font-size: 16px;
                font-weight: 700;
                padding: 12px 22px;
            }
        )CSS");
        gtk_style_context_add_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
        g_object_unref(provider);
    }

    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(
        keys,
        "key-pressed",
        G_CALLBACK(+[](
            GtkEventControllerKey*,
            guint keyval,
            guint,
            GdkModifierType,
            gpointer data
        ) -> gboolean {
            auto* self = static_cast<PowerMenuOverlay*>(data);
            if (keyval == GDK_KEY_Escape) {
                self->hide();
                return GDK_EVENT_STOP;
            }
            return GDK_EVENT_PROPAGATE;
        }),
        this
    );
    gtk_widget_add_controller(GTK_WIDGET(window_), keys);

    gtk_window_set_child(window_, root);
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

PowerMenuOverlay::~PowerMenuOverlay() {
    cancel_interaction_setup();
    clear_confirmation();
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
        window_ = nullptr;
    }
}

void PowerMenuOverlay::show() {
    clear_confirmation();
    gtk_window_present(window_);
    schedule_interaction_setup();
}

void PowerMenuOverlay::hide() {
    cancel_interaction_setup();
    clear_confirmation();
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

void PowerMenuOverlay::schedule_interaction_setup() {
    cancel_interaction_setup();
    input_region_commit_frames_remaining_ = kInputRegionCommitFrames;
    interaction_setup_tick_id_ = gtk_widget_add_tick_callback(
        GTK_WIDGET(window_),
        +[](GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
            auto* self = static_cast<PowerMenuOverlay*>(data);
            if (self->advance_interaction_setup(widget)) {
                return G_SOURCE_CONTINUE;
            }
            self->interaction_setup_tick_id_ = 0;
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

void PowerMenuOverlay::cancel_interaction_setup() {
    if (interaction_setup_tick_id_ == 0 || window_ == nullptr) return;
    gtk_widget_remove_tick_callback(
        GTK_WIDGET(window_),
        interaction_setup_tick_id_
    );
    interaction_setup_tick_id_ = 0;
    input_region_commit_frames_remaining_ = 0;
}

bool PowerMenuOverlay::advance_interaction_setup(GtkWidget* widget) {
    if (!visible()) return false;

    const bool input_ready = apply_full_input_region(widget);
    const bool controls_ready = controls_ != nullptr &&
        controls_->prepare_for_interaction();
    if (!input_ready || !controls_ready) return true;

    gtk_widget_queue_draw(widget);
    --input_region_commit_frames_remaining_;
    return input_region_commit_frames_remaining_ > 0;
}

void PowerMenuOverlay::toggle() {
    if (visible()) {
        hide();
    } else {
        show();
    }
}

bool PowerMenuOverlay::visible() const {
    return gtk_widget_get_visible(GTK_WIDGET(window_));
}

void PowerMenuOverlay::activate(Action action) {
    const auto result = confirmation_.activate(
        action,
        PowerMenuConfirmation::Clock::now()
    );
    if (result == PowerMenuConfirmation::Result::Armed) {
        show_confirmation(action);
        return;
    }

    clear_confirmation();
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    if (preview_mode_enabled()) {
        std::cerr << "[PowerMenu] Preview confirmation: " << action_name(action) << '\n';
        return;
    }

    std::function<bool()>* callback = nullptr;
    switch (action) {
        case Action::Lock: callback = &actions_.lock; break;
        case Action::Suspend: callback = &actions_.suspend; break;
        case Action::Logout: callback = &actions_.logout; break;
        case Action::Reboot: callback = &actions_.reboot; break;
        case Action::PowerOff: callback = &actions_.power_off; break;
    }
    if (callback == nullptr || !*callback || !(*callback)()) {
        std::cerr << "[PowerMenu] Unable to " << action_name(action) << '\n';
    }
}

void PowerMenuOverlay::show_confirmation(Action action) {
    controls_->set_armed(action);
    const std::string message = std::string{action_label(action)} +
        " ARMED — CLICK AGAIN NOW TO CONFIRM";
    gtk_label_set_text(GTK_LABEL(confirmation_banner_), message.c_str());
    gtk_widget_set_visible(confirmation_banner_, TRUE);

    if (confirmation_timeout_id_ != 0) {
        g_source_remove(confirmation_timeout_id_);
    }
    confirmation_timeout_id_ = g_timeout_add(
        kConfirmationTimeoutMs,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<PowerMenuOverlay*>(data);
            self->confirmation_timeout_id_ = 0;
            self->clear_confirmation();
            return G_SOURCE_REMOVE;
        },
        this
    );
}

void PowerMenuOverlay::clear_confirmation() {
    if (confirmation_timeout_id_ != 0) {
        g_source_remove(confirmation_timeout_id_);
        confirmation_timeout_id_ = 0;
    }
    confirmation_.cancel();
    if (controls_ != nullptr) controls_->set_armed(std::nullopt);
    if (confirmation_banner_ != nullptr) {
        gtk_widget_set_visible(confirmation_banner_, FALSE);
    }
}

} // namespace realmheart::ui::powermenu