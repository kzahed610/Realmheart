#include "ui/components/ToggleWidget.hpp"
#include <gtk/gtk.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>

namespace realmheart::ui::components {

ToggleWidget::ToggleWidget(const std::string& label, bool initial, std::function<bool(bool)> on_toggle)
    : worker_state_(std::make_shared<WorkerState>()) {
    
    worker_state_->on_toggle = std::move(on_toggle);

    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    GtkWidget* lbl_name = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
    gtk_box_append(GTK_BOX(box_), lbl_name);

    switch_ = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(switch_), initial);
    g_signal_connect(switch_, "state-set", G_CALLBACK(+[](GtkSwitch*, gboolean state, gpointer data) -> gboolean {
        auto* self = static_cast<ToggleWidget*>(data);
        if (self->updating_) return FALSE;

        {
            std::lock_guard lock(self->worker_state_->mutex);
            self->worker_state_->target_state = state;
            self->worker_state_->has_pending = true;
        }
        self->worker_state_->cv.notify_one();
        return FALSE;
    }), this);
    gtk_box_append(GTK_BOX(box_), switch_);

    // Initialize provider once
    provider_ = gtk_css_provider_new();
    gtk_style_context_add_provider(gtk_widget_get_style_context(box_), 
                                   GTK_STYLE_PROVIDER(provider_), 
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

ToggleWidget::~ToggleWidget() {
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

void ToggleWidget::refresh() {
    if (worker_.joinable()) return;

    const auto state = worker_state_;
    worker_ = std::thread([state, this] {
        while (true) {
            bool state_to_set = false;
            {
                std::unique_lock lock(state->mutex);
                state->cv.wait(lock, [&state] { return state->shutdown || state->has_pending; });
                if (state->shutdown) return;
                state_to_set = state->target_state;
                state->has_pending = false;
            }

            bool succeeded = false;
            try {
                succeeded = state->on_toggle(state_to_set);
            } catch (const std::exception&) {
                succeeded = false;
            }

            if (!succeeded) {
                struct AsyncState {
                    ToggleWidget* module;
                    bool requested_state;
                };
                g_idle_add(+[](gpointer data) -> gboolean {
                    std::unique_ptr<AsyncState> result(static_cast<AsyncState*>(data));
                    if (result->module) {
                        result->module->set_active(!result->requested_state);
                    }
                    return G_SOURCE_REMOVE;
                }, new AsyncState{this, state_to_set});
            }
        }
    });
}

void ToggleWidget::apply_theme(const services::Palette& palette) {
    std::string text_color = palette.get("text", "#cdd6f4");
    
    std::string css = ".toggle-widget { color: " + text_color + "; }";
    
    gtk_css_provider_load_from_string(provider_, css.c_str());
    
    gtk_widget_add_css_class(box_, "toggle-widget");
}

} // namespace realmheart::ui::components
