#include "ui/sidebar/NightLightPanel.hpp"

#include "ui/sidebar/VerticalRevealClip.hpp"

#include "core/TaskExecutor.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace realmheart::ui::sidebar {
namespace {

using namespace std::chrono_literals;

GtkWidget* make_icon(const char* path, int pixels, const char* css_class = nullptr) {
    realmheart::ui::bar::widgets::ThemedSvgIcon icon(path, pixels);
    if (css_class != nullptr) icon.add_css_class(css_class);
    return icon.widget();
}

GtkWidget* make_label(const char* text, const char* css_class = nullptr) {
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    if (css_class != nullptr) gtk_widget_add_css_class(label, css_class);
    return label;
}

realmheart::core::CommandOptions night_light_options() {
    realmheart::core::CommandOptions options;
    options.deadline = 4s;
    options.terminate_grace = 250ms;
    return options;
}

} // namespace

struct NightLightPanel::LifetimeState {
    std::atomic<bool> alive{true};
    std::atomic<std::uint64_t> generation{0};
    std::mutex operation_mutex;
    NightLightPanel* owner = nullptr; // GTK main thread only
};

NightLightPanel::NightLightPanel(
    GtkWidget* overlay_host,
    std::function<void()> state_changed
) : overlay_host_(overlay_host),
    state_changed_(std::move(state_changed)),
    lifetime_(std::make_shared<LifetimeState>()) {
    lifetime_->owner = this;
    build();
}

NightLightPanel::~NightLightPanel() {
    if (debounce_source_ != 0) {
        g_source_remove(debounce_source_);
        debounce_source_ = 0;
    }
    lifetime_->alive = false;
    lifetime_->owner = nullptr;
    if (overlay_host_ != nullptr && revealer_ != nullptr) {
        gtk_overlay_remove_overlay(GTK_OVERLAY(overlay_host_), revealer_);
    }
    revealer_ = nullptr;
    content_ = nullptr;
}

void NightLightPanel::build() {
    content_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(content_, "realmheart-connectivity-panel");
    gtk_widget_add_css_class(content_, "realmheart-night-light-panel");
    gtk_widget_set_size_request(content_, 342, 276);
    gtk_widget_set_overflow(content_, GTK_OVERFLOW_HIDDEN);

    revealer_ = realmheart_vertical_reveal_clip_new(content_, 1010, 960, 16);
    gtk_widget_add_css_class(revealer_, "realmheart-connectivity-revealer");
    gtk_widget_set_halign(revealer_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(revealer_, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(revealer_, FALSE);
    gtk_widget_set_visible(revealer_, FALSE);
    g_signal_connect(revealer_, "concealed", G_CALLBACK(+[](
        RealmheartVerticalRevealClip*, gpointer data
    ) {
        auto* self = static_cast<NightLightPanel*>(data);
        if (!self->requested_visible_ && self->revealer_ != nullptr) {
            gtk_widget_set_visible(self->revealer_, FALSE);
        }
    }), this);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_host_), revealer_);
    gtk_overlay_set_clip_overlay(GTK_OVERLAY(overlay_host_), revealer_, TRUE);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "realmheart-manager-header");
    gtk_box_append(GTK_BOX(header), make_icon(
        "Realmheart-Icons/night-light.svg", 20
    ));
    GtkWidget* heading = make_label("NIGHT LIGHT", "realmheart-manager-title");
    gtk_widget_set_hexpand(heading, TRUE);
    gtk_box_append(GTK_BOX(header), heading);
    GtkWidget* close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(close, "realmheart-manager-close-button");
    g_signal_connect(close, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<NightLightPanel*>(data)->hide();
    }), this);
    gtk_box_append(GTK_BOX(header), close);
    gtk_box_append(GTK_BOX(content_), header);

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(body, "realmheart-night-light-body");

    GtkWidget* enable_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(enable_row, "realmheart-night-light-enable-row");
    GtkWidget* enable_copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(enable_copy, TRUE);
    gtk_box_append(GTK_BOX(enable_copy), make_label(
        "Blue-light filter", "realmheart-night-light-control-title"
    ));
    GtkWidget* description = make_label(
        "Warm the display without affecting screenshots.",
        "realmheart-night-light-control-detail"
    );
    gtk_label_set_wrap(GTK_LABEL(description), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(description), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(enable_copy), description);
    gtk_box_append(GTK_BOX(enable_row), enable_copy);

    toggle_ = gtk_toggle_button_new_with_label("OFF");
    gtk_widget_add_css_class(toggle_, "realmheart-night-light-toggle");
    g_signal_connect(toggle_, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
        auto* self = static_cast<NightLightPanel*>(data);
        if (self->updating_) return;
        self->set_enabled(gtk_toggle_button_get_active(button));
    }), this);
    gtk_box_append(GTK_BOX(enable_row), toggle_);
    gtk_box_append(GTK_BOX(body), enable_row);

    GtkWidget* strength_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(strength_card, "realmheart-night-light-strength-card");
    GtkWidget* strength_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* strength_title = make_label(
        "STRENGTH", "realmheart-night-light-strength-title"
    );
    gtk_widget_set_hexpand(strength_title, TRUE);
    gtk_box_append(GTK_BOX(strength_header), strength_title);
    strength_value_ = make_label("57%", "realmheart-night-light-strength-value");
    gtk_label_set_xalign(GTK_LABEL(strength_value_), 1.0F);
    gtk_box_append(GTK_BOX(strength_header), strength_value_);
    gtk_box_append(GTK_BOX(strength_card), strength_header);

    scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_widget_add_css_class(scale_, "realmheart-rune-scale");
    gtk_widget_add_css_class(scale_, "realmheart-night-light-scale");
    gtk_scale_set_draw_value(GTK_SCALE(scale_), FALSE);
    gtk_range_set_value(GTK_RANGE(scale_), pending_strength_);
    gtk_widget_set_hexpand(scale_, TRUE);
    g_signal_connect(scale_, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer data) {
        auto* self = static_cast<NightLightPanel*>(data);
        if (self->updating_) return;

        self->pending_strength_ = static_cast<int>(std::lround(
            gtk_range_get_value(range)
        ));
        self->update_strength_copy(self->pending_strength_);
        if (!self->enabled_) return;

        if (self->debounce_source_ != 0) g_source_remove(self->debounce_source_);
        self->debounce_source_ = g_timeout_add(
            130,
            +[](gpointer raw) -> gboolean {
                auto* panel = static_cast<NightLightPanel*>(raw);
                panel->debounce_source_ = 0;
                panel->set_temperature(
                    services::NightLight::strength_to_temperature(
                        panel->pending_strength_
                    )
                );
                return G_SOURCE_REMOVE;
            },
            self
        );
    }), this);
    gtk_box_append(GTK_BOX(strength_card), scale_);

    GtkWidget* scale_legend = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* subtle = make_label("Subtle", "realmheart-night-light-scale-legend");
    gtk_widget_set_hexpand(subtle, TRUE);
    gtk_box_append(GTK_BOX(scale_legend), subtle);
    GtkWidget* warm = make_label(
        "Maximum warmth", "realmheart-night-light-scale-legend"
    );
    gtk_label_set_xalign(GTK_LABEL(warm), 1.0F);
    gtk_box_append(GTK_BOX(scale_legend), warm);
    gtk_box_append(GTK_BOX(strength_card), scale_legend);

    temperature_value_ = make_label(
        "4000 K", "realmheart-night-light-temperature"
    );
    gtk_widget_set_halign(temperature_value_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(strength_card), temperature_value_);
    gtk_box_append(GTK_BOX(body), strength_card);
    gtk_box_append(GTK_BOX(content_), body);

    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_add_css_class(footer, "realmheart-night-light-footer");
    spinner_ = gtk_spinner_new();
    gtk_widget_set_size_request(spinner_, 14, 14);
    gtk_box_append(GTK_BOX(footer), spinner_);
    status_ = make_label("Ready", "realmheart-manager-status");
    gtk_widget_set_hexpand(status_, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(footer), status_);
    gtk_box_append(GTK_BOX(content_), footer);

    render(services::NightLightState{});
}

void NightLightPanel::show() {
    if (visible()) return;
    requested_visible_ = true;
    gtk_widget_set_visible(revealer_, TRUE);
    gtk_widget_set_can_target(revealer_, TRUE);
    realmheart_vertical_reveal_clip_set_revealed(
        REALMHEART_VERTICAL_REVEAL_CLIP(revealer_), TRUE
    );
    refresh();
}

void NightLightPanel::hide() {
    if (!visible()) return;
    requested_visible_ = false;
    gtk_widget_set_can_target(revealer_, FALSE);
    realmheart_vertical_reveal_clip_set_revealed(
        REALMHEART_VERTICAL_REVEAL_CLIP(revealer_), FALSE
    );
}

bool NightLightPanel::visible() const {
    return requested_visible_;
}

GtkWidget* NightLightPanel::widget() const {
    return revealer_;
}

void NightLightPanel::toggle() {
    if (visible()) hide();
    else show();
}

void NightLightPanel::refresh() {
    const auto lifetime = lifetime_;
    const std::uint64_t generation = lifetime->generation.fetch_add(1) + 1;
    set_busy(true);
    realmheart::core::shared_task_executor().post([lifetime, generation] {
        std::optional<services::NightLightState> state;
        {
            std::lock_guard lock(lifetime->operation_mutex);
            if (!lifetime->alive.load() || lifetime->generation.load() != generation) return;
            state = services::NightLight::read(night_light_options());
        }
        struct Result {
            std::shared_ptr<LifetimeState> lifetime;
            std::uint64_t generation;
            std::optional<services::NightLightState> state;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* result = static_cast<Result*>(raw);
                auto& lifetime = *result->lifetime;
                if (lifetime.alive.load() && lifetime.owner != nullptr &&
                    lifetime.generation.load() == result->generation) {
                    if (result->state) {
                        lifetime.owner->render(*result->state);
                    } else {
                        lifetime.owner->set_busy(false);
                        lifetime.owner->set_status("hyprsunset is unavailable", true);
                    }
                }
                return G_SOURCE_REMOVE;
            },
            new Result{lifetime, generation, std::move(state)},
            +[](gpointer raw) { delete static_cast<Result*>(raw); }
        );
    });
}

void NightLightPanel::render(const services::NightLightState& state) {
    enabled_ = state.enabled;
    pending_strength_ = services::NightLight::temperature_to_strength(
        state.temperature
    );

    updating_ = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle_), enabled_);
    gtk_button_set_label(GTK_BUTTON(toggle_), enabled_ ? "ON" : "OFF");
    gtk_widget_remove_css_class(toggle_, "active");
    if (enabled_) gtk_widget_add_css_class(toggle_, "active");
    gtk_range_set_value(GTK_RANGE(scale_), pending_strength_);
    updating_ = false;

    update_strength_copy(pending_strength_);
    gtk_widget_set_sensitive(scale_, enabled_);
    set_busy(false);
    set_status(
        enabled_
            ? "Night Light is active"
            : "Enable Night Light to adjust its strength"
    );
}

void NightLightPanel::set_enabled(bool enabled) {
    const int temperature = services::NightLight::strength_to_temperature(
        pending_strength_
    );
    run_mutation([enabled, temperature] {
        return services::NightLight::set_enabled(
            enabled, temperature, night_light_options()
        );
    });
}

void NightLightPanel::set_temperature(int temperature) {
    run_mutation([temperature] {
        return services::NightLight::set_temperature(
            temperature, night_light_options()
        );
    });
}

void NightLightPanel::run_mutation(
    std::function<services::NightLightMutationResult()> mutation
) {
    const auto lifetime = lifetime_;
    const std::uint64_t generation = lifetime->generation.fetch_add(1) + 1;
    set_busy(true);
    realmheart::core::shared_task_executor().post([
        lifetime, generation, mutation = std::move(mutation)
    ] {
        services::NightLightMutationResult result;
        std::optional<services::NightLightState> fallback;
        {
            std::lock_guard lock(lifetime->operation_mutex);
            if (!lifetime->alive.load() || lifetime->generation.load() != generation) return;
            try {
                result = mutation
                    ? mutation()
                    : services::NightLightMutationResult{};
            } catch (const std::exception& exception) {
                result.error = exception.what();
            }
            if (!result.success) {
                fallback = services::NightLight::read(night_light_options());
            }
        }
        struct Result {
            std::shared_ptr<LifetimeState> lifetime;
            std::uint64_t generation;
            services::NightLightMutationResult mutation;
            std::optional<services::NightLightState> fallback;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* result = static_cast<Result*>(raw);
                auto& lifetime = *result->lifetime;
                if (lifetime.alive.load() && lifetime.owner != nullptr &&
                    lifetime.generation.load() == result->generation) {
                    auto* owner = lifetime.owner;
                    if (result->mutation.success) {
                        owner->render(result->mutation.state);
                        if (owner->state_changed_) owner->state_changed_();
                    } else {
                        if (result->fallback) owner->render(*result->fallback);
                        else owner->set_busy(false);
                        owner->set_status(
                            result->mutation.error.empty()
                                ? "Night Light action failed"
                                : result->mutation.error,
                            true
                        );
                    }
                }
                return G_SOURCE_REMOVE;
            },
            new Result{
                lifetime,
                generation,
                std::move(result),
                std::move(fallback)
            },
            +[](gpointer raw) { delete static_cast<Result*>(raw); }
        );
    });
}

void NightLightPanel::update_strength_copy(int strength) {
    const int temperature = services::NightLight::strength_to_temperature(strength);
    const std::string percent = std::to_string(strength) + "%";
    const std::string kelvin = std::to_string(temperature) + " K";
    gtk_label_set_text(GTK_LABEL(strength_value_), percent.c_str());
    gtk_label_set_text(GTK_LABEL(temperature_value_), kelvin.c_str());
}

void NightLightPanel::set_busy(bool busy) {
    gtk_widget_set_sensitive(toggle_, !busy);
    gtk_widget_set_sensitive(scale_, !busy && enabled_);
    if (busy) gtk_spinner_start(GTK_SPINNER(spinner_));
    else gtk_spinner_stop(GTK_SPINNER(spinner_));
}

void NightLightPanel::set_status(const std::string& message, bool error) {
    gtk_label_set_text(GTK_LABEL(status_), message.c_str());
    gtk_widget_remove_css_class(status_, "error");
    if (error) gtk_widget_add_css_class(status_, "error");
}

} // namespace realmheart::ui::sidebar
