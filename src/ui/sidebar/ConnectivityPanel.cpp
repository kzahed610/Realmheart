#include "ui/sidebar/ConnectivityPanel.hpp"

#include "core/TaskExecutor.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace realmheart::ui::sidebar {
namespace {

using namespace std::chrono_literals;

void clear_box(GtkWidget* box) {
    GtkWidget* child = gtk_widget_get_first_child(box);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

GtkWidget* make_icon(const char* path, int pixels, const char* css_class = nullptr) {
    realmheart::ui::bar::widgets::ThemedSvgIcon icon(path, pixels);
    if (css_class != nullptr) icon.add_css_class(css_class);
    return icon.widget();
}

GtkWidget* make_text_label(const std::string& text, const char* css_class = nullptr) {
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    if (css_class != nullptr) gtk_widget_add_css_class(label, css_class);
    return label;
}

void set_error_class(GtkWidget* label, bool error) {
    gtk_widget_remove_css_class(label, "error");
    if (error) gtk_widget_add_css_class(label, "error");
}

realmheart::core::CommandOptions network_options() {
    realmheart::core::CommandOptions options;
    options.deadline = 20s;
    options.terminate_grace = 250ms;
    return options;
}

std::string wifi_detail(const services::WifiNetwork& network) {
    std::string detail;
    if (network.active) detail = "Connected";
    else if (network.saved) detail = "Saved";
    else detail = network.secured() ? "Secured" : "Open";
    detail += "  •  " + std::to_string(network.signal_percent) + "%";
    if (!network.security.empty() && network.security != "--") {
        detail += "  •  " + network.security;
    }
    return detail;
}

std::string bluetooth_detail(const services::BluetoothDevice& device) {
    if (device.connected) return "Connected";
    if (device.paired) return "Paired";
    return "Available";
}

} // namespace

struct WifiManagerPopover::LifetimeState {
    std::atomic<bool> alive{true};
    std::atomic<std::uint64_t> generation{0};
    std::mutex operation_mutex;
    WifiManagerPopover* owner = nullptr; // GTK main thread only
};

struct WifiManagerPopover::RowAction {
    enum class Kind { Connect, Disconnect, Forget };
    WifiManagerPopover* owner = nullptr;
    services::WifiNetwork network;
    Kind kind = Kind::Connect;
};

WifiManagerPopover::WifiManagerPopover(
    GtkWidget* overlay_host,
    std::function<void()> state_changed
) : overlay_host_(overlay_host),
    state_changed_(std::move(state_changed)),
    lifetime_(std::make_shared<LifetimeState>()) {
    lifetime_->owner = this;
    build();
}

WifiManagerPopover::~WifiManagerPopover() {
    lifetime_->alive = false;
    lifetime_->owner = nullptr;
    if (overlay_host_ != nullptr && revealer_ != nullptr) {
        gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_host_), revealer_);
    }
    if (overlay_host_ != nullptr && backdrop_ != nullptr) {
        gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_host_), backdrop_);
    }
    revealer_ = nullptr;
    backdrop_ = nullptr;
    content_ = nullptr;
}

void WifiManagerPopover::build() {
    backdrop_ = gtk_button_new();
    gtk_widget_add_css_class(backdrop_, "realmheart-connectivity-backdrop");
    gtk_widget_set_halign(backdrop_, GTK_ALIGN_FILL);
    gtk_widget_set_valign(backdrop_, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(backdrop_, TRUE);
    gtk_widget_set_vexpand(backdrop_, TRUE);
    gtk_widget_set_visible(backdrop_, FALSE);
    g_signal_connect(backdrop_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<WifiManagerPopover*>(data)->hide();
    }), this);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_host_), backdrop_);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay_host_), backdrop_, TRUE);

    revealer_ = gtk_revealer_new();
    gtk_widget_add_css_class(revealer_, "realmheart-connectivity-revealer");
    gtk_revealer_set_transition_type(
        GTK_REVEALER(revealer_), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE
    );
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer_), 150);
    gtk_widget_set_halign(revealer_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(revealer_, GTK_ALIGN_CENTER);
    // CROSSFADE revealers keep their full allocation even while visually
    // hidden. Disable hit testing until the panel is actually shown so the
    // transparent panel cannot block controls beneath it.
    gtk_widget_set_can_target(revealer_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_host_), revealer_);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay_host_), revealer_, TRUE);

    content_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(content_, "realmheart-connectivity-panel");
    gtk_widget_set_size_request(content_, 304, 356);
    gtk_widget_set_overflow(content_, GTK_OVERFLOW_HIDDEN);
    gtk_revealer_set_child(GTK_REVEALER(revealer_), content_);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "realmheart-manager-header");
    gtk_box_append(GTK_BOX(header), make_icon("Realmheart-Icons/wifi.svg", 20));
    GtkWidget* heading = make_text_label("WI-FI NETWORKS", "realmheart-manager-title");
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(heading), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(header), heading);

    GtkWidget* close_button = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close_button, "realmheart-manager-close-button");
    g_signal_connect(close_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<WifiManagerPopover*>(data)->hide();
    }), this);
    gtk_box_append(GTK_BOX(header), close_button);
    gtk_box_append(GTK_BOX(content_), header);

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(toolbar, "realmheart-manager-toolbar");
    status_ = make_text_label("Ready", "realmheart-manager-status");
    gtk_widget_set_hexpand(status_, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(status_), 22);
    gtk_box_append(GTK_BOX(toolbar), status_);

    spinner_ = gtk_spinner_new();
    gtk_widget_set_visible(spinner_, FALSE);
    gtk_box_append(GTK_BOX(toolbar), spinner_);

    GtkWidget* refresh_button = gtk_button_new_with_label("Refresh");
    gtk_widget_add_css_class(refresh_button, "realmheart-manager-quiet-button");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<WifiManagerPopover*>(data)->refresh(true);
    }), this);
    gtk_box_append(GTK_BOX(toolbar), refresh_button);

    power_button_ = gtk_button_new_with_label("Turn off");
    gtk_widget_add_css_class(power_button_, "realmheart-manager-quiet-button");
    g_signal_connect(power_button_, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
        auto* self = static_cast<WifiManagerPopover*>(data);
        const bool currently_on = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(button), "realmheart-power-state")
        ) != 0;
        self->set_powered(!currently_on);
    }), this);
    gtk_box_append(GTK_BOX(toolbar), power_button_);
    gtk_box_append(GTK_BOX(content_), toolbar);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "realmheart-manager-scroller");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC
    );
    gtk_widget_set_vexpand(scroller, TRUE);
    list_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(list_, "realmheart-manager-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list_);
    gtk_box_append(GTK_BOX(content_), scroller);

    password_revealer_ = gtk_revealer_new();
    gtk_revealer_set_transition_type(
        GTK_REVEALER(password_revealer_), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP
    );
    GtkWidget* password_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_add_css_class(password_box, "realmheart-password-panel");
    password_title_ = make_text_label("Connect to network", "realmheart-password-title");
    gtk_label_set_ellipsize(GTK_LABEL(password_title_), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(password_box), password_title_);

    password_entry_ = gtk_password_entry_new();
    gtk_widget_add_css_class(password_entry_, "realmheart-password-entry");
    gtk_widget_set_hexpand(password_entry_, TRUE);
    g_object_set(password_entry_, "placeholder-text", "Network password", nullptr);
    gtk_box_append(GTK_BOX(password_box), password_entry_);

    GtkWidget* password_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_halign(password_actions, GTK_ALIGN_END);
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(cancel, "realmheart-manager-quiet-button");
    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<WifiManagerPopover*>(data)->hide_password_prompt();
    }), this);
    gtk_box_append(GTK_BOX(password_actions), cancel);
    GtkWidget* connect = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(connect, "realmheart-manager-primary-button");
    g_signal_connect(connect, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<WifiManagerPopover*>(data);
        if (!self->pending_network_) return;
        const char* value = gtk_editable_get_text(GTK_EDITABLE(self->password_entry_));
        const std::string password(value != nullptr ? value : "");
        if (password.empty()) {
            self->set_status("Enter the network password", true);
            return;
        }
        self->connect_network(*self->pending_network_, password);
    }), this);
    gtk_box_append(GTK_BOX(password_actions), connect);
    gtk_box_append(GTK_BOX(password_box), password_actions);
    gtk_revealer_set_child(GTK_REVEALER(password_revealer_), password_box);
    gtk_box_append(GTK_BOX(content_), password_revealer_);
}

void WifiManagerPopover::show() {
    if (visible()) return;
    gtk_widget_set_visible(backdrop_, TRUE);
    gtk_widget_set_can_target(revealer_, TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer_), TRUE);
    refresh(true);
}

void WifiManagerPopover::hide() {
    hide_password_prompt();
    // Stop intercepting clicks immediately; the fade-out can continue
    // visually without leaving an invisible input shield over the sidebar.
    gtk_widget_set_can_target(revealer_, FALSE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer_), FALSE);
    gtk_widget_set_visible(backdrop_, FALSE);
}

bool WifiManagerPopover::visible() const {
    return revealer_ != nullptr &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(revealer_));
}

void WifiManagerPopover::toggle() {
    if (visible()) hide();
    else show();
}

void WifiManagerPopover::refresh(bool rescan) {
    const auto lifetime = lifetime_;
    const std::uint64_t generation = lifetime->generation.fetch_add(1) + 1;
    set_busy(true, rescan ? "Scanning for nearby networks…" : "Refreshing network state…");
    realmheart::core::shared_task_executor().post([lifetime, generation, rescan] {
        std::optional<services::WifiState> state;
        std::vector<services::WifiNetwork> networks;
        {
            std::lock_guard lock(lifetime->operation_mutex);
            if (!lifetime->alive.load() || lifetime->generation.load() != generation) return;
            const auto options = network_options();
            state = services::Wifi::read(options);
            if (state && state->enabled) {
                networks = services::Wifi::scan(rescan, options);
            }
        }

        struct Result {
            std::shared_ptr<LifetimeState> lifetime;
            std::uint64_t generation;
            std::optional<services::WifiState> state;
            std::vector<services::WifiNetwork> networks;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* result = static_cast<Result*>(raw);
                auto& lifetime = *result->lifetime;
                if (lifetime.alive.load() && lifetime.owner != nullptr &&
                    lifetime.generation.load() == result->generation) {
                    lifetime.owner->render(result->state, result->networks);
                    lifetime.owner->set_busy(false);
                }
                return G_SOURCE_REMOVE;
            },
            new Result{lifetime, generation, std::move(state), std::move(networks)},
            +[](gpointer raw) { delete static_cast<Result*>(raw); }
        );
    });
}

void WifiManagerPopover::render(
    const std::optional<services::WifiState>& state,
    const std::vector<services::WifiNetwork>& networks
) {
    clear_box(list_);
    const bool powered = state.has_value() && state->enabled;
    g_object_set_data(
        G_OBJECT(power_button_), "realmheart-power-state", GINT_TO_POINTER(powered ? 1 : 0)
    );
    gtk_button_set_label(GTK_BUTTON(power_button_), powered ? "Turn off" : "Turn on");

    if (!state) {
        set_status("NetworkManager is unavailable", true);
        GtkWidget* empty = make_text_label("Wi-Fi could not be queried.", "realmheart-manager-empty");
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(list_), empty);
        return;
    }
    if (!powered) {
        set_status("Wi-Fi is off");
        GtkWidget* empty = make_text_label("Turn Wi-Fi on to discover networks.", "realmheart-manager-empty");
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(list_), empty);
        return;
    }

    if (!state->ssid.empty()) set_status("Connected to " + state->ssid);
    else set_status("Select a network to connect");
    if (networks.empty()) {
        GtkWidget* empty = make_text_label("No networks found", "realmheart-manager-empty");
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(list_), empty);
        return;
    }

    for (const auto& network : networks) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "realmheart-manager-row");
        if (network.active) gtk_widget_add_css_class(row, "active");
        gtk_box_append(GTK_BOX(row), make_icon("Realmheart-Icons/wifi.svg", 21));

        GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(labels, TRUE);
        gtk_widget_set_size_request(labels, 0, -1);
        GtkWidget* name = make_text_label(network.ssid, "realmheart-manager-row-title");
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 16);
        gtk_box_append(GTK_BOX(labels), name);
        GtkWidget* detail = make_text_label(
            wifi_detail(network), "realmheart-manager-row-detail"
        );
        gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(detail), 19);
        gtk_box_append(GTK_BOX(labels), detail);
        gtk_box_append(GTK_BOX(row), labels);

        GtkWidget* action = gtk_button_new_with_label(network.active ? "Disconnect" : "Connect");
        gtk_widget_add_css_class(action, network.active
            ? "realmheart-manager-quiet-button"
            : "realmheart-manager-primary-button");
        auto* action_data = new RowAction{
            .owner = this,
            .network = network,
            .kind = network.active ? RowAction::Kind::Disconnect : RowAction::Kind::Connect,
        };
        g_object_set_data_full(
            G_OBJECT(action), "realmheart-wifi-row-action", action_data,
            +[](gpointer raw) { delete static_cast<RowAction*>(raw); }
        );
        g_signal_connect(action, "clicked", G_CALLBACK(on_row_action), action_data);
        gtk_box_append(GTK_BOX(row), action);

        if (network.saved) {
            GtkWidget* forget = gtk_button_new_with_label("Forget");
            gtk_widget_add_css_class(forget, "realmheart-manager-forget-button");
            auto* forget_data = new RowAction{
                .owner = this,
                .network = network,
                .kind = RowAction::Kind::Forget,
            };
            g_object_set_data_full(
                G_OBJECT(forget), "realmheart-wifi-forget-action", forget_data,
                +[](gpointer raw) { delete static_cast<RowAction*>(raw); }
            );
            g_signal_connect(forget, "clicked", G_CALLBACK(on_row_action), forget_data);
            gtk_box_append(GTK_BOX(row), forget);
        }
        gtk_box_append(GTK_BOX(list_), row);
    }
}

void WifiManagerPopover::on_row_action(GtkButton*, gpointer data) {
    auto* action = static_cast<RowAction*>(data);
    if (action == nullptr || action->owner == nullptr) return;
    switch (action->kind) {
    case RowAction::Kind::Connect:
        if (action->network.saved || !action->network.secured()) {
            action->owner->connect_network(action->network, std::nullopt);
        } else {
            action->owner->show_password_prompt(action->network);
        }
        break;
    case RowAction::Kind::Disconnect:
        action->owner->disconnect_network();
        break;
    case RowAction::Kind::Forget:
        action->owner->forget_network(action->network);
        break;
    }
}

void WifiManagerPopover::show_password_prompt(const services::WifiNetwork& network) {
    pending_network_ = network;
    gtk_label_set_text(
        GTK_LABEL(password_title_), ("Connect to “" + network.ssid + "”").c_str()
    );
    gtk_editable_set_text(GTK_EDITABLE(password_entry_), "");
    gtk_revealer_set_reveal_child(GTK_REVEALER(password_revealer_), TRUE);
    gtk_widget_grab_focus(password_entry_);
}

void WifiManagerPopover::hide_password_prompt() {
    pending_network_.reset();
    gtk_editable_set_text(GTK_EDITABLE(password_entry_), "");
    gtk_revealer_set_reveal_child(GTK_REVEALER(password_revealer_), FALSE);
}

void WifiManagerPopover::connect_network(
    const services::WifiNetwork& network,
    std::optional<std::string> password
) {
    hide_password_prompt();
    run_action("Connecting to " + network.ssid + "…", [network, password = std::move(password)] {
        const auto mutation = services::Wifi::connect(
            network.ssid, password, network.connection_uuid, network_options()
        );
        return mutation.success ? std::string{} : mutation.error;
    });
}

void WifiManagerPopover::disconnect_network() {
    run_action("Disconnecting…", [] {
        const auto mutation = services::Wifi::disconnect(network_options());
        return mutation.success ? std::string{} : mutation.error;
    });
}

void WifiManagerPopover::forget_network(const services::WifiNetwork& network) {
    run_action("Forgetting " + network.ssid + "…", [network] {
        const auto mutation = services::Wifi::forget(
            network.ssid, network.connection_uuid, network_options()
        );
        return mutation.success ? std::string{} : mutation.error;
    });
}

void WifiManagerPopover::set_powered(bool enabled) {
    run_action(enabled ? "Turning Wi-Fi on…" : "Turning Wi-Fi off…", [enabled] {
        const auto mutation = services::Wifi::set_enabled(enabled, network_options());
        return mutation.success ? std::string{} : mutation.error;
    });
}

void WifiManagerPopover::run_action(
    std::string progress,
    std::function<std::string()> action
) {
    const auto lifetime = lifetime_;
    const std::uint64_t generation = lifetime->generation.fetch_add(1) + 1;
    set_busy(true, progress);
    realmheart::core::shared_task_executor().post([
        lifetime, generation, action = std::move(action)
    ] {
        std::string error;
        {
            std::lock_guard lock(lifetime->operation_mutex);
            if (!lifetime->alive.load() || lifetime->generation.load() != generation) return;
            try {
                error = action ? action() : "Network action is unavailable";
            } catch (const std::exception& exception) {
                error = exception.what();
            }
        }
        struct Result {
            std::shared_ptr<LifetimeState> lifetime;
            std::uint64_t generation;
            std::string error;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* result = static_cast<Result*>(raw);
                auto& lifetime = *result->lifetime;
                if (lifetime.alive.load() && lifetime.owner != nullptr &&
                    lifetime.generation.load() == result->generation) {
                    if (!result->error.empty()) {
                        lifetime.owner->set_busy(false);
                        lifetime.owner->set_status(result->error, true);
                    } else {
                        if (lifetime.owner->state_changed_) lifetime.owner->state_changed_();
                        lifetime.owner->refresh(false);
                    }
                }
                return G_SOURCE_REMOVE;
            },
            new Result{lifetime, generation, std::move(error)},
            +[](gpointer raw) { delete static_cast<Result*>(raw); }
        );
    });
}

void WifiManagerPopover::set_busy(bool busy, const std::string& message) {
    gtk_widget_set_sensitive(list_, !busy);
    gtk_widget_set_sensitive(power_button_, !busy);
    gtk_widget_set_visible(spinner_, busy);
    if (busy) gtk_spinner_start(GTK_SPINNER(spinner_));
    else gtk_spinner_stop(GTK_SPINNER(spinner_));
    if (!message.empty()) set_status(message);
}

void WifiManagerPopover::set_status(const std::string& message, bool error) {
    gtk_label_set_text(GTK_LABEL(status_), message.c_str());
    set_error_class(status_, error);
}

struct BluetoothManagerPopover::LifetimeState {
    std::atomic<bool> alive{true};
    std::atomic<std::uint64_t> generation{0};
    std::mutex operation_mutex;
    BluetoothManagerPopover* owner = nullptr; // GTK main thread only
};

struct BluetoothManagerPopover::RowAction {
    enum class Kind { Connect, Disconnect, Forget };
    BluetoothManagerPopover* owner = nullptr;
    services::BluetoothDevice device;
    Kind kind = Kind::Connect;
};

BluetoothManagerPopover::BluetoothManagerPopover(
    GtkWidget* overlay_host,
    std::function<void()> state_changed
) : overlay_host_(overlay_host),
    state_changed_(std::move(state_changed)),
    lifetime_(std::make_shared<LifetimeState>()) {
    lifetime_->owner = this;
    build();
}

BluetoothManagerPopover::~BluetoothManagerPopover() {
    lifetime_->alive = false;
    lifetime_->owner = nullptr;
    if (overlay_host_ != nullptr && revealer_ != nullptr) {
        gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_host_), revealer_);
    }
    if (overlay_host_ != nullptr && backdrop_ != nullptr) {
        gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_host_), backdrop_);
    }
    revealer_ = nullptr;
    backdrop_ = nullptr;
    content_ = nullptr;
}

void BluetoothManagerPopover::build() {
    backdrop_ = gtk_button_new();
    gtk_widget_add_css_class(backdrop_, "realmheart-connectivity-backdrop");
    gtk_widget_set_halign(backdrop_, GTK_ALIGN_FILL);
    gtk_widget_set_valign(backdrop_, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(backdrop_, TRUE);
    gtk_widget_set_vexpand(backdrop_, TRUE);
    gtk_widget_set_visible(backdrop_, FALSE);
    g_signal_connect(backdrop_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<BluetoothManagerPopover*>(data)->hide();
    }), this);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_host_), backdrop_);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay_host_), backdrop_, TRUE);

    revealer_ = gtk_revealer_new();
    gtk_widget_add_css_class(revealer_, "realmheart-connectivity-revealer");
    gtk_revealer_set_transition_type(
        GTK_REVEALER(revealer_), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE
    );
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer_), 150);
    gtk_widget_set_halign(revealer_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(revealer_, GTK_ALIGN_CENTER);
    // CROSSFADE revealers keep their full allocation even while visually
    // hidden. Disable hit testing until the panel is actually shown so the
    // transparent panel cannot block controls beneath it.
    gtk_widget_set_can_target(revealer_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_host_), revealer_);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay_host_), revealer_, TRUE);

    content_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(content_, "realmheart-connectivity-panel");
    gtk_widget_set_size_request(content_, 304, 356);
    gtk_widget_set_overflow(content_, GTK_OVERFLOW_HIDDEN);
    gtk_revealer_set_child(GTK_REVEALER(revealer_), content_);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "realmheart-manager-header");
    gtk_box_append(GTK_BOX(header), make_icon("Realmheart-Icons/bluetooth.svg", 20));
    GtkWidget* heading = make_text_label("BLUETOOTH DEVICES", "realmheart-manager-title");
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(heading), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(header), heading);

    GtkWidget* close_button = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close_button, "realmheart-manager-close-button");
    g_signal_connect(close_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<BluetoothManagerPopover*>(data)->hide();
    }), this);
    gtk_box_append(GTK_BOX(header), close_button);
    gtk_box_append(GTK_BOX(content_), header);

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(toolbar, "realmheart-manager-toolbar");
    status_ = make_text_label("Ready", "realmheart-manager-status");
    gtk_widget_set_hexpand(status_, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(status_), 22);
    gtk_box_append(GTK_BOX(toolbar), status_);

    spinner_ = gtk_spinner_new();
    gtk_widget_set_visible(spinner_, FALSE);
    gtk_box_append(GTK_BOX(toolbar), spinner_);

    GtkWidget* refresh_button = gtk_button_new_with_label("Scan");
    gtk_widget_add_css_class(refresh_button, "realmheart-manager-quiet-button");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<BluetoothManagerPopover*>(data)->refresh(true);
    }), this);
    gtk_box_append(GTK_BOX(toolbar), refresh_button);

    power_button_ = gtk_button_new_with_label("Turn off");
    gtk_widget_add_css_class(power_button_, "realmheart-manager-quiet-button");
    g_signal_connect(power_button_, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
        auto* self = static_cast<BluetoothManagerPopover*>(data);
        const bool powered = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(button), "realmheart-power-state")
        ) != 0;
        self->set_powered(!powered);
    }), this);
    gtk_box_append(GTK_BOX(toolbar), power_button_);
    gtk_box_append(GTK_BOX(content_), toolbar);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "realmheart-manager-scroller");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC
    );
    gtk_widget_set_vexpand(scroller, TRUE);
    list_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_add_css_class(list_, "realmheart-manager-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list_);
    gtk_box_append(GTK_BOX(content_), scroller);
}

void BluetoothManagerPopover::show() {
    if (visible()) return;
    gtk_widget_set_visible(backdrop_, TRUE);
    gtk_widget_set_can_target(revealer_, TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer_), TRUE);
    refresh(true);
}

void BluetoothManagerPopover::hide() {
    // Stop intercepting clicks immediately; the fade-out can continue
    // visually without leaving an invisible input shield over the sidebar.
    gtk_widget_set_can_target(revealer_, FALSE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer_), FALSE);
    gtk_widget_set_visible(backdrop_, FALSE);
}

bool BluetoothManagerPopover::visible() const {
    return revealer_ != nullptr &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(revealer_));
}

void BluetoothManagerPopover::toggle() {
    if (visible()) hide();
    else show();
}

void BluetoothManagerPopover::refresh(bool scan_for_new_devices) {
    const auto lifetime = lifetime_;
    const std::uint64_t generation = lifetime->generation.fetch_add(1) + 1;
    set_busy(true, scan_for_new_devices ? "Scanning for nearby devices…" : "Refreshing devices…");
    realmheart::core::shared_task_executor().post([
        lifetime, generation, scan_for_new_devices
    ] {
        std::optional<services::BluetoothState> state;
        std::vector<services::BluetoothDevice> devices;
        {
            std::lock_guard lock(lifetime->operation_mutex);
            if (!lifetime->alive.load() || lifetime->generation.load() != generation) return;
            const auto options = network_options();
            state = services::Bluetooth::read(options);
            if (state && state->powered) {
                devices = services::Bluetooth::devices(scan_for_new_devices, options);
            }
        }
        struct Result {
            std::shared_ptr<LifetimeState> lifetime;
            std::uint64_t generation;
            std::optional<services::BluetoothState> state;
            std::vector<services::BluetoothDevice> devices;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* result = static_cast<Result*>(raw);
                auto& lifetime = *result->lifetime;
                if (lifetime.alive.load() && lifetime.owner != nullptr &&
                    lifetime.generation.load() == result->generation) {
                    lifetime.owner->render(result->state, result->devices);
                    lifetime.owner->set_busy(false);
                }
                return G_SOURCE_REMOVE;
            },
            new Result{lifetime, generation, std::move(state), std::move(devices)},
            +[](gpointer raw) { delete static_cast<Result*>(raw); }
        );
    });
}

void BluetoothManagerPopover::render(
    const std::optional<services::BluetoothState>& state,
    const std::vector<services::BluetoothDevice>& devices
) {
    clear_box(list_);
    const bool powered = state.has_value() && state->powered;
    g_object_set_data(
        G_OBJECT(power_button_), "realmheart-power-state", GINT_TO_POINTER(powered ? 1 : 0)
    );
    gtk_button_set_label(GTK_BUTTON(power_button_), powered ? "Turn off" : "Turn on");

    if (!state) {
        set_status("Bluetooth is unavailable", true);
        GtkWidget* empty = make_text_label("No Bluetooth controller found.", "realmheart-manager-empty");
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(list_), empty);
        return;
    }
    if (!powered) {
        set_status("Bluetooth is off");
        GtkWidget* empty = make_text_label("Turn Bluetooth on to discover devices.", "realmheart-manager-empty");
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(list_), empty);
        return;
    }
    set_status("Select a device to connect");
    if (devices.empty()) {
        GtkWidget* empty = make_text_label("No devices found", "realmheart-manager-empty");
        gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(list_), empty);
        return;
    }

    for (const auto& device : devices) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "realmheart-manager-row");
        if (device.connected) gtk_widget_add_css_class(row, "active");
        gtk_box_append(GTK_BOX(row), make_icon("Realmheart-Icons/bluetooth.svg", 21));

        GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(labels, TRUE);
        gtk_widget_set_size_request(labels, 0, -1);
        GtkWidget* name = make_text_label(device.name, "realmheart-manager-row-title");
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 16);
        gtk_box_append(GTK_BOX(labels), name);
        gtk_box_append(GTK_BOX(labels), make_text_label(
            bluetooth_detail(device), "realmheart-manager-row-detail"
        ));
        gtk_box_append(GTK_BOX(row), labels);

        GtkWidget* action = gtk_button_new_with_label(device.connected ? "Disconnect" : "Connect");
        gtk_widget_add_css_class(action, device.connected
            ? "realmheart-manager-quiet-button"
            : "realmheart-manager-primary-button");
        auto* action_data = new RowAction{
            .owner = this,
            .device = device,
            .kind = device.connected ? RowAction::Kind::Disconnect : RowAction::Kind::Connect,
        };
        g_object_set_data_full(
            G_OBJECT(action), "realmheart-bluetooth-row-action", action_data,
            +[](gpointer raw) { delete static_cast<RowAction*>(raw); }
        );
        g_signal_connect(action, "clicked", G_CALLBACK(on_row_action), action_data);
        gtk_box_append(GTK_BOX(row), action);

        if (device.paired) {
            GtkWidget* forget = gtk_button_new_with_label("Forget");
            gtk_widget_add_css_class(forget, "realmheart-manager-forget-button");
            auto* forget_data = new RowAction{
                .owner = this,
                .device = device,
                .kind = RowAction::Kind::Forget,
            };
            g_object_set_data_full(
                G_OBJECT(forget), "realmheart-bluetooth-forget-action", forget_data,
                +[](gpointer raw) { delete static_cast<RowAction*>(raw); }
            );
            g_signal_connect(forget, "clicked", G_CALLBACK(on_row_action), forget_data);
            gtk_box_append(GTK_BOX(row), forget);
        }
        gtk_box_append(GTK_BOX(list_), row);
    }
}

void BluetoothManagerPopover::on_row_action(GtkButton*, gpointer data) {
    auto* action = static_cast<RowAction*>(data);
    if (action == nullptr || action->owner == nullptr) return;
    switch (action->kind) {
    case RowAction::Kind::Connect:
        action->owner->connect_device(action->device);
        break;
    case RowAction::Kind::Disconnect:
        action->owner->disconnect_device(action->device);
        break;
    case RowAction::Kind::Forget:
        action->owner->forget_device(action->device);
        break;
    }
}

void BluetoothManagerPopover::connect_device(const services::BluetoothDevice& device) {
    run_action("Connecting to " + device.name + "…", [device] {
        const auto mutation = services::Bluetooth::connect(device.address, network_options());
        return mutation.success ? std::string{} : mutation.error;
    });
}

void BluetoothManagerPopover::disconnect_device(const services::BluetoothDevice& device) {
    run_action("Disconnecting " + device.name + "…", [device] {
        const auto mutation = services::Bluetooth::disconnect(device.address, network_options());
        return mutation.success ? std::string{} : mutation.error;
    });
}

void BluetoothManagerPopover::forget_device(const services::BluetoothDevice& device) {
    run_action("Forgetting " + device.name + "…", [device] {
        const auto mutation = services::Bluetooth::forget(device.address, network_options());
        return mutation.success ? std::string{} : mutation.error;
    });
}

void BluetoothManagerPopover::set_powered(bool powered) {
    run_action(powered ? "Turning Bluetooth on…" : "Turning Bluetooth off…", [powered] {
        const auto mutation = services::Bluetooth::set_powered(powered, network_options());
        return mutation.success ? std::string{} : mutation.error;
    });
}

void BluetoothManagerPopover::run_action(
    std::string progress,
    std::function<std::string()> action
) {
    const auto lifetime = lifetime_;
    const std::uint64_t generation = lifetime->generation.fetch_add(1) + 1;
    set_busy(true, progress);
    realmheart::core::shared_task_executor().post([
        lifetime, generation, action = std::move(action)
    ] {
        std::string error;
        {
            std::lock_guard lock(lifetime->operation_mutex);
            if (!lifetime->alive.load() || lifetime->generation.load() != generation) return;
            try {
                error = action ? action() : "Bluetooth action is unavailable";
            } catch (const std::exception& exception) {
                error = exception.what();
            }
        }
        struct Result {
            std::shared_ptr<LifetimeState> lifetime;
            std::uint64_t generation;
            std::string error;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* result = static_cast<Result*>(raw);
                auto& lifetime = *result->lifetime;
                if (lifetime.alive.load() && lifetime.owner != nullptr &&
                    lifetime.generation.load() == result->generation) {
                    if (!result->error.empty()) {
                        lifetime.owner->set_busy(false);
                        lifetime.owner->set_status(result->error, true);
                    } else {
                        if (lifetime.owner->state_changed_) lifetime.owner->state_changed_();
                        lifetime.owner->refresh(false);
                    }
                }
                return G_SOURCE_REMOVE;
            },
            new Result{lifetime, generation, std::move(error)},
            +[](gpointer raw) { delete static_cast<Result*>(raw); }
        );
    });
}

void BluetoothManagerPopover::set_busy(bool busy, const std::string& message) {
    gtk_widget_set_sensitive(list_, !busy);
    gtk_widget_set_sensitive(power_button_, !busy);
    gtk_widget_set_visible(spinner_, busy);
    if (busy) gtk_spinner_start(GTK_SPINNER(spinner_));
    else gtk_spinner_stop(GTK_SPINNER(spinner_));
    if (!message.empty()) set_status(message);
}

void BluetoothManagerPopover::set_status(const std::string& message, bool error) {
    gtk_label_set_text(GTK_LABEL(status_), message.c_str());
    set_error_class(status_, error);
}

} // namespace realmheart::ui::sidebar
