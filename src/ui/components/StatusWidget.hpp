#pragma once

#include "ui/components/BaseWidget.hpp"

#include <functional>
#include <string>

namespace realmheart::ui::components {

class StatusWidget : public BaseWidget {
public:
    struct Slot {
        std::string name;
        std::string icon_name;
        std::string fallback_text;
        std::string tooltip;
        std::string badge_text;
        bool enabled = false;
    };

    StatusWidget(Slot slot, std::function<void()> on_click);
    ~StatusWidget() override;

    GtkWidget* get_widget() override;
    void set_status(const Slot& new_slot);

private:
    void update_badge();

    GtkWidget* button_ = nullptr;
    GtkWidget* overlay_ = nullptr;
    GtkWidget* image_ = nullptr;
    GtkWidget* badge_ = nullptr;
    gulong click_handler_ = 0;
    Slot slot_;
    std::function<void()> on_click_;
};

} // namespace realmheart::ui::components
