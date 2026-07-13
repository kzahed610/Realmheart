#include "ui/components/ToggleWidget.hpp"

#include "core/TaskExecutor.hpp"

#include <exception>
#include <utility>

namespace realmheart::ui::components {

ToggleWidget::ToggleWidget(
    std::string label,
    bool initial,
    std::function<bool(bool)> on_toggle
) : state_(std::make_shared<AsyncState>()) {
    state_->on_toggle = std::move(on_toggle);

    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(box_, "realmheart-module-row");
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    GtkWidget* name = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(box_), name);

    switch_ = gtk_switch_new();
    state_->switch_widget = switch_;
    gtk_switch_set_active(GTK_SWITCH(switch_), initial);
    state_->signal_handler = g_signal_connect(
        switch_,
        "state-set",
        G_CALLBACK(+[](GtkSwitch*, gboolean requested, gpointer data) -> gboolean {
            auto* self = static_cast<ToggleWidget*>(data);
            if (self->updating_) return FALSE;

            const auto state = self->state_;
            const std::uint64_t generation = state->generation.fetch_add(1) + 1;
            realmheart::core::shared_task_executor().post([state, requested = requested != FALSE, generation] {
                bool succeeded = false;
                {
                    // Serialize this toggle's mutations. Generation checks alone only
                    // protect the UI; without this lock two worker threads could leave
                    // the underlying service in the older state.
                    std::lock_guard operation_lock(state->mutation_mutex);
                    if (!state->alive.load() || state->generation.load() != generation) return;
                    try {
                        succeeded = state->on_toggle && state->on_toggle(requested);
                    } catch (const std::exception&) {
                        succeeded = false;
                    }
                }

                struct Result {
                    std::shared_ptr<AsyncState> state;
                    std::uint64_t generation;
                    bool requested;
                    bool succeeded;
                };
                g_idle_add_full(
                    G_PRIORITY_DEFAULT_IDLE,
                    +[](gpointer raw) -> gboolean {
                        auto* result = static_cast<Result*>(raw);
                        auto& state = *result->state;
                        if (!state.alive.load() || state.switch_widget == nullptr ||
                            state.generation.load() != result->generation) {
                            return G_SOURCE_REMOVE;
                        }
                        if (!result->succeeded) {
                            if (state.signal_handler != 0) {
                                g_signal_handler_block(state.switch_widget, state.signal_handler);
                            }
                            gtk_switch_set_active(
                                GTK_SWITCH(state.switch_widget),
                                !result->requested
                            );
                            if (state.signal_handler != 0) {
                                g_signal_handler_unblock(state.switch_widget, state.signal_handler);
                            }
                        }
                        return G_SOURCE_REMOVE;
                    },
                    new Result{state, generation, requested, succeeded},
                    +[](gpointer raw) { delete static_cast<Result*>(raw); }
                );
            });
            return FALSE;
        }),
        this
    );
    gtk_box_append(GTK_BOX(box_), switch_);
}

ToggleWidget::~ToggleWidget() {
    state_->alive = false;
    state_->switch_widget = nullptr;
    if (switch_ != nullptr && state_->signal_handler != 0) {
        g_signal_handler_disconnect(switch_, state_->signal_handler);
        state_->signal_handler = 0;
    }
}

GtkWidget* ToggleWidget::get_widget() {
    return box_;
}

void ToggleWidget::set_active(bool active) {
    updating_ = true;
    gtk_switch_set_active(GTK_SWITCH(switch_), active);
    updating_ = false;
}

} // namespace realmheart::ui::components
