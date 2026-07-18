#include "ui/components/SliderWidget.hpp"

#include "core/TaskExecutor.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <cmath>
#include <exception>
#include <string>
#include <utility>

namespace realmheart::ui::components {
namespace {

std::string value_text(double value) {
    return std::to_string(static_cast<int>(std::lround(value))) + "%";
}

GtkWidget* slider_icon(const std::string& label) {
    const char* path = label == "Brightness"
        ? "Realmheart-Icons/brightness.svg"
        : "Realmheart-Icons/speaker-2.svg";
    realmheart::ui::bar::widgets::ThemedSvgIcon icon(path, 17);
    icon.add_css_class("realmheart-slider-icon");
    return icon.widget();
}

} // namespace

SliderWidget::SliderWidget(
    std::string label,
    double min,
    double max,
    double initial,
    Mutator on_change,
    ConfirmedCallback on_confirmed
) : pending_value_(initial), state_(std::make_shared<AsyncState>()) {
    state_->on_change = std::move(on_change);
    state_->on_confirmed = std::move(on_confirmed);
    state_->confirmed_value.store(initial);

    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_add_css_class(box_, "realmheart-module-slider");
    gtk_box_append(GTK_BOX(box_), slider_icon(label));

    GtkWidget* name = gtk_label_new(label.c_str());
    gtk_widget_add_css_class(name, "realmheart-slider-label");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
    gtk_widget_set_size_request(name, 69, -1);
    gtk_box_append(GTK_BOX(box_), name);

    scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1.0);
    gtk_widget_add_css_class(scale_, "realmheart-rune-scale");
    gtk_scale_set_draw_value(GTK_SCALE(scale_), FALSE);
    gtk_widget_set_hexpand(scale_, TRUE);
    state_->scale = scale_;
    gtk_range_set_value(GTK_RANGE(scale_), initial);
    value_label_ = gtk_label_new(value_text(initial).c_str());
    gtk_widget_add_css_class(value_label_, "realmheart-slider-value");
    gtk_label_set_xalign(GTK_LABEL(value_label_), 1.0F);
    gtk_widget_set_size_request(value_label_, 34, -1);
    state_->value_label = value_label_;
    state_->value_changed_handler = g_signal_connect(
        scale_,
        "value-changed",
        G_CALLBACK(+[](GtkRange* range, gpointer data) {
            auto* self = static_cast<SliderWidget*>(data);
            if (self->updating_) return;

            self->pending_value_ = gtk_range_get_value(range);
            self->show_interaction_feedback();
            gtk_label_set_text(
                GTK_LABEL(self->value_label_), value_text(self->pending_value_).c_str()
            );
            if (self->debounce_source_ != 0) g_source_remove(self->debounce_source_);
            self->debounce_source_ = g_timeout_add(
                120,
                +[](gpointer callback_data) -> gboolean {
                    auto* widget = static_cast<SliderWidget*>(callback_data);
                    widget->debounce_source_ = 0;

                    const auto state = widget->state_;
                    const double requested = widget->pending_value_;
                    const std::uint64_t generation = state->generation.fetch_add(1) + 1;
                    realmheart::core::shared_task_executor().post([state, requested, generation] {
                        std::optional<double> actual;
                        {
                            // Preserve mutation order even though the shared pool has
                            // multiple workers. Stale queued slider commits are skipped;
                            // an in-flight older commit is always followed by the newest.
                            std::lock_guard operation_lock(state->mutation_mutex);
                            if (!state->alive.load() || state->generation.load() != generation) return;
                            try {
                                actual = state->on_change
                                    ? state->on_change(requested)
                                    : std::optional<double>{requested};
                                if (actual) state->confirmed_value.store(*actual);
                            } catch (const std::exception&) {
                                actual = std::nullopt;
                            }
                        }

                        struct Result {
                            std::shared_ptr<AsyncState> state;
                            std::uint64_t generation;
                            std::optional<double> actual;
                        };
                        g_idle_add_full(
                            G_PRIORITY_DEFAULT_IDLE,
                            +[](gpointer data) -> gboolean {
                                auto* result = static_cast<Result*>(data);
                                auto& state = *result->state;
                                if (!state.alive.load() || state.scale == nullptr ||
                                    state.generation.load() != result->generation) {
                                    return G_SOURCE_REMOVE;
                                }

                                if (state.value_changed_handler != 0) {
                                    g_signal_handler_block(state.scale, state.value_changed_handler);
                                }
                                gtk_range_set_value(
                                    GTK_RANGE(state.scale),
                                    state.confirmed_value.load()
                                );
                                if (state.value_label != nullptr) {
                                    gtk_label_set_text(
                                        GTK_LABEL(state.value_label),
                                        value_text(state.confirmed_value.load()).c_str()
                                    );
                                }
                                if (state.value_changed_handler != 0) {
                                    g_signal_handler_unblock(state.scale, state.value_changed_handler);
                                }
                                if (result->actual && state.on_confirmed) {
                                    state.on_confirmed(*result->actual);
                                }
                                return G_SOURCE_REMOVE;
                            },
                            new Result{state, generation, std::move(actual)},
                            +[](gpointer data) { delete static_cast<Result*>(data); }
                        );
                    });
                    return G_SOURCE_REMOVE;
                },
                self
            );
        }),
        this
    );
    gtk_box_append(GTK_BOX(box_), scale_);
    gtk_box_append(GTK_BOX(box_), value_label_);
}

SliderWidget::~SliderWidget() {
    if (interaction_feedback_source_ != 0) {
        g_source_remove(interaction_feedback_source_);
        interaction_feedback_source_ = 0;
    }
    if (debounce_source_ != 0) {
        g_source_remove(debounce_source_);
        debounce_source_ = 0;
    }
    state_->alive = false;
    state_->scale = nullptr;
    state_->value_label = nullptr;
    if (scale_ != nullptr && state_->value_changed_handler != 0) {
        g_signal_handler_disconnect(scale_, state_->value_changed_handler);
        state_->value_changed_handler = 0;
    }
}

void SliderWidget::show_interaction_feedback() {
    if (box_ == nullptr) return;

    if (interaction_feedback_source_ != 0) {
        g_source_remove(interaction_feedback_source_);
        interaction_feedback_source_ = 0;
    }

    gtk_widget_add_css_class(box_, "interacting");
    interaction_feedback_source_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        170,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<SliderWidget*>(data);
            self->interaction_feedback_source_ = 0;
            if (self->box_ != nullptr) {
                gtk_widget_remove_css_class(self->box_, "interacting");
            }
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

GtkWidget* SliderWidget::get_widget() {
    return box_;
}

void SliderWidget::set_value(double value) {
    state_->confirmed_value.store(value);
    pending_value_ = value;
    updating_ = true;
    gtk_range_set_value(GTK_RANGE(scale_), value);
    if (available_) {
        gtk_label_set_text(GTK_LABEL(value_label_), value_text(value).c_str());
    }
    updating_ = false;
}

void SliderWidget::set_available(bool available) {
    available_ = available;
    gtk_widget_set_sensitive(scale_, available);
    gtk_widget_remove_css_class(box_, "unavailable");
    if (!available) {
        gtk_widget_add_css_class(box_, "unavailable");
        gtk_label_set_text(GTK_LABEL(value_label_), "—");
        return;
    }
    gtk_label_set_text(
        GTK_LABEL(value_label_), value_text(state_->confirmed_value.load()).c_str()
    );
}

} // namespace realmheart::ui::components
