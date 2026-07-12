#include "ui/components/LabelWidget.hpp"

#include <exception>
#include <utility>

namespace realmheart::ui::components {

LabelWidget::LabelWidget(std::string label, std::string initial_value, Reader reader)
    : reader_(std::move(reader)), state_(std::make_shared<AsyncState>()) {
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

    state_->value_label = gtk_label_new(initial_value.c_str());
    gtk_widget_add_css_class(state_->value_label, "realmheart-module-value");
    gtk_label_set_xalign(GTK_LABEL(state_->value_label), 1.0F);
    gtk_box_append(GTK_BOX(box_), state_->value_label);
}

LabelWidget::~LabelWidget() {
    state_->alive = false;
    state_->value_label = nullptr;
    if (worker_.joinable()) worker_.join();
}

GtkWidget* LabelWidget::get_widget() {
    return box_;
}

void LabelWidget::set_value(const std::string& value) {
    if (state_->value_label != nullptr) {
        gtk_label_set_text(GTK_LABEL(state_->value_label), value.c_str());
    }
}

void LabelWidget::refresh() {
    if (!reader_) return;

    if (worker_.joinable()) {
        if (state_->refresh_in_flight.load()) return;
        worker_.join();
    }

    if (state_->refresh_in_flight.exchange(true)) return;
    const Reader reader = reader_;
    const auto state = state_;

    worker_ = std::thread([state, reader] {
        std::string value;
        try {
            value = reader();
        } catch (const std::exception&) {
            value = "Unavailable";
        }

        struct Result {
            std::shared_ptr<AsyncState> state;
            std::string value;
        };

        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer data) -> gboolean {
                auto* result = static_cast<Result*>(data);
                if (result->state->alive.load() && result->state->value_label != nullptr) {
                    gtk_label_set_text(
                        GTK_LABEL(result->state->value_label),
                        result->value.c_str()
                    );
                }
                result->state->refresh_in_flight = false;
                return G_SOURCE_REMOVE;
            },
            new Result{state, std::move(value)},
            +[](gpointer data) { delete static_cast<Result*>(data); }
        );
    });
}

} // namespace realmheart::ui::components
