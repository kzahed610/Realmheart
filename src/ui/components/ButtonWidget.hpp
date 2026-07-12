#pragma once

#include "ui/components/BaseWidget.hpp"

#include <functional>
#include <string>

namespace realmheart::ui::components {

class ButtonWidget : public BaseWidget {
public:
    ButtonWidget(std::string label, std::function<void()> on_click);
    ~ButtonWidget() override;

    GtkWidget* get_widget() override;

private:
    GtkWidget* box_ = nullptr;
    GtkWidget* button_ = nullptr;
    gulong click_handler_ = 0;
    std::function<void()> on_click_;
};

} // namespace realmheart::ui::components
