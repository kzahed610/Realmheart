#pragma once

#include "ui/components/BaseWidget.hpp"

#include <functional>
#include <optional>
#include <string>

namespace realmheart::ui::components {

class SliderWidget : public BaseWidget {
public:
    SliderWidget(
        std::string label,
        double min,
        double max,
        double initial,
        std::function<std::optional<double>(double)> on_change
    );
    ~SliderWidget() override;

    GtkWidget* get_widget() override;
    void set_value(double value);

private:
    GtkWidget* box_ = nullptr;
    GtkWidget* scale_ = nullptr;
    gulong value_changed_handler_ = 0;
    std::function<std::optional<double>(double)> on_change_;
    bool updating_ = false;
    double pending_value_ = 0.0;
    double confirmed_value_ = 0.0;
    guint debounce_source_ = 0;
};

} // namespace realmheart::ui::components
