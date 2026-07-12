#include "ui/components/ToggleWidget.hpp"

#include <exception>
#include <utility>

namespace realmheart::ui::components {

ToggleWidget::ToggleWidget(
    std::string label,
    bool initial,
    std::function<bool(bool)> on_toggle
) : worker_state_(std::make_shared<WorkerState>()) {
    worker_state_->on_toggle = std::move(on_toggle);

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
    worker_state_->switch_widget = switch_;
    gtk_switch_set_active(GTK_SWITCH(switch_), initial);
    worker_state_->signal_handler = g_signal_connect(
        switch_,
        "state-set",
        G_CALLBACK(+[](GtkSwitch*, gboolean state, gpointer data) -> gboolean {
            auto* self = static_cast<ToggleWidget*>(data);
            if (self->updating_) return FALSE;

            {
                std::lock_guard lock(self->worker_state_->mutex);
                self->worker_state_->target_state = state;
                self->worker_state_->has_pending = true;
            }
            self->worker_state_->cv.notify_one();
            return FALSE;
        }),
        this
    );
    gtk_box_append(GTK_BOX(box_), switch_);

    start_worker();
}

ToggleWidget::~ToggleWidget() {
    if (switch_ != nullptr && worker_state_->signal_handler != 0) {
        g_signal_handler_disconnect(switch_, worker_state_->signal_handler);
        worker_state_->signal_handler = 0;
    }

    worker_state_->alive = false;
    worker_state_->switch_widget = nullptr;
    {
        std::lock_guard lock(worker_state_->mutex);
        worker_state_->shutdown = true;
    }
    worker_state_->cv.notify_one();
    if (worker_.joinable()) worker_.join();
}

GtkWidget* ToggleWidget::get_widget() {
    return box_;
}

void ToggleWidget::set_active(bool active) {
    updating_ = true;
    gtk_switch_set_active(GTK_SWITCH(switch_), active);
    updating_ = false;
}

void ToggleWidget::start_worker() {
    const auto state = worker_state_;
    worker_ = std::thread([state] {
        while (true) {
            bool requested_state = false;
            {
                std::unique_lock lock(state->mutex);
                state->cv.wait(lock, [&state] {
                    return state->shutdown || state->has_pending;
                });
                if (state->shutdown) return;
                requested_state = state->target_state;
                state->has_pending = false;
            }

            bool succeeded = false;
            try {
                succeeded = state->on_toggle && state->on_toggle(requested_state);
            } catch (const std::exception&) {
                succeeded = false;
            }

            if (succeeded) continue;

            struct Result {
                std::shared_ptr<WorkerState> state;
                bool requested_state;
            };
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer data) -> gboolean {
                    auto* result = static_cast<Result*>(data);
                    auto& state = *result->state;
                    if (!state.alive.load() || state.switch_widget == nullptr) {
                        return G_SOURCE_REMOVE;
                    }
                    if (state.signal_handler != 0) {
                        g_signal_handler_block(state.switch_widget, state.signal_handler);
                    }
                    gtk_switch_set_active(
                        GTK_SWITCH(state.switch_widget),
                        !result->requested_state
                    );
                    if (state.signal_handler != 0) {
                        g_signal_handler_unblock(state.switch_widget, state.signal_handler);
                    }
                    return G_SOURCE_REMOVE;
                },
                new Result{state, requested_state},
                +[](gpointer data) { delete static_cast<Result*>(data); }
            );
        }
    });
}

} // namespace realmheart::ui::components
