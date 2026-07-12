#pragma once

#include "ui/components/BaseWidget.hpp"
#include <gtk/gtk.h>
#include <string>
#include <functional>
#include <optional>

namespace realmheart::ui::components {

class SliderWidget : public ThemeableWidget {
public:
    SliderWidget(
        const std::string& label,
        double min,
        double max,
        double initial,
        std::function<std::optional<double>(double)> on_change
    );
    ~SliderWidget() override;

    GtkWidget* get_widget() override;
    void refresh() override;
    void set_value(double value);
    void apply_theme(const services::Palette& palette) override;

private:
    GtkWidget* box_ = nullptr;
    GtkWidget* scale_ = nullptr;
    std::function<std::optional<double>(double)> on_change_;
    
    bool updating_ = false;
    double pending_value_ = 0.0;
    double confirmed_value_ = 0.0;
    guint debounce_source_ = 0;
};

} // namespace realmheart::ui::components
