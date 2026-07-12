#pragma once

#include "ui/components/BaseWidget.hpp"
#include <gtk/gtk.h>
#include <string>
#include <functional>

namespace realmheart::ui::components {

class ButtonWidget : public ThemeableWidget {
public:
    ButtonWidget(const std::string& label, std::function<void()> on_click);
    ~ButtonWidget() override = default;

    GtkWidget* get_widget() override;
    void apply_theme(const services::Palette& palette) override;

private:
    GtkWidget* box_ = nullptr;
    GtkWidget* btn_ = nullptr;
    std::function<void()> on_click_;
};

} // namespace realmheart::ui::components
