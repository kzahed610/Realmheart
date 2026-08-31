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

GtkWidget* slider_icon(const std::string& label, int pixels) {
    const char* path = label == "Brightness"
        ? "Realmheart-Icons/brightness.svg"
        : "Realmheart-Icons/speaker-2.svg";
    realmheart::ui::bar::widgets::ThemedSvgIcon icon(path, pixels);
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
    icon_ = slider_icon(label, SliderLayout{}.icon_size);
    gtk_box_append(GTK_BOX(box_), icon_);

    name_label_ = gtk_label_new(label.c_str());
    gtk_widget_add_css_class(name_label_, "realmheart-slider-label");
    gtk_label_set_xalign(GTK_LABEL(name_label_), 0.0F);
    gtk_box_append(GTK_BOX(box_), name_label_);

    scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1.0);
    gtk_widget_add_css_class(scale_, "realmheart-rune-scale");
    gtk_scale_set_draw_value(GTK_SCALE(scale_), FALSE);
    gtk_widget_set_hexpand(scale_, TRUE);
    state_->scale = scale_;
    gtk_range_set_value(GTK_RANGE(scale_), initial);
    value_label_ = gtk_label_new(value_text(initial).c_str());
    gtk_widget_add_css_class(value_label_, "realmheart-slider-value");
    gtk_label_set_xalign(GTK_LABEL(value_label_), 1.0F);
    state_->value_label = value_label_;
    state_->value_changed_handler = g_signal_connect(
        scale_,
        "value-changed",
        G_CALLBACK(+[](GtkRange* range, gpointer data) {
            auto* self = static_cast<SliderWidget*>(data);
            if (self->updating_) return;

            self->pending_value_ = gtk_range_get_value(range);
            self->state_->mutation_pending = true;
            self->pending_generation_ = self->state_->generation.fetch_add(1) + 1;
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
                    const std::uint64_t generation = widget->pending_generation_;
                    const bool posted = realmheart::core::shared_task_executor().post([state, requested, generation] {
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
                                // Invalidate reads that began while this mutation was
                                // pending before allowing fresh reads to apply.
                                state.generation.fetch_add(1);
                                state.mutation_pending = false;
                                if (result->actual && state.on_confirmed) {
                                    state.on_confirmed(*result->actual);
                                }
                                return G_SOURCE_REMOVE;
                            },
                            new Result{state, generation, std::move(actual)},
                            +[](gpointer data) { delete static_cast<Result*>(data); }
                        );
                    });
                    if (!posted && state->generation.load() == generation) {
                        if (state->value_changed_handler != 0) {
                            g_signal_handler_block(
                                state->scale, state->value_changed_handler
                            );
                        }
                        gtk_range_set_value(
                            GTK_RANGE(state->scale), state->confirmed_value.load()
                        );
                        if (state->value_label != nullptr) {
                            gtk_label_set_text(
                                GTK_LABEL(state->value_label),
                                value_text(state->confirmed_value.load()).c_str()
                            );
                        }
                        if (state->value_changed_handler != 0) {
                            g_signal_handler_unblock(
                                state->scale, state->value_changed_handler
                            );
                        }
                        state->generation.fetch_add(1);
                        state->mutation_pending = false;
                    }
                    return G_SOURCE_REMOVE;
                },
                self
            );
        }),
        this
    );
    gtk_box_append(GTK_BOX(box_), scale_);
    gtk_box_append(GTK_BOX(box_), value_label_);
    set_layout(SliderLayout{});
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

void SliderWidget::set_layout(SliderLayout layout) {
    gtk_box_set_spacing(GTK_BOX(box_), layout.row_spacing);
    gtk_widget_set_size_request(box_, -1, layout.row_height);
    if (icon_ != nullptr) {
        gtk_widget_set_size_request(icon_, layout.icon_size, layout.icon_size);
    }
    if (name_label_ != nullptr) {
        gtk_widget_set_size_request(name_label_, layout.label_width, -1);
    }
    if (value_label_ != nullptr) {
        gtk_widget_set_size_request(value_label_, layout.value_width, -1);
    }
    gtk_widget_queue_resize(box_);
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

std::uint64_t SliderWidget::refresh_generation() const noexcept {
    return state_->generation.load();
}

void SliderWidget::apply_refresh(
    std::optional<double> value,
    std::uint64_t generation
) {
    if (state_->generation.load() != generation || state_->mutation_pending.load()) return;
    set_available(value.has_value());
    if (value) set_value(*value);
}

} // namespace realmheart::ui::components
