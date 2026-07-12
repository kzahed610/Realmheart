#pragma once

#include "ui/components/BaseWidget.hpp"
#include <gtk/gtk.h>
#include <string>
#include <functional>
#include <vector>
#include <memory>

namespace realmheart::ui::components {

class StatusWidget : public ThemeableWidget {
public:
    struct Slot {
        std::string name;
        std::string icon_name;
        std::string fallback_text;
        std::string tooltip;
        std::string badge_text;
        bool enabled = false;
    };

    StatusWidget(const Slot& slot, std::function<void()> on_click);
    ~StatusWidget() override = default;

    GtkWidget* get_widget() override;
    void refresh() override;
    void set_status(const Slot& new_slot);
    void apply_theme(const services::Palette& palette) override;

private:
    GtkWidget* box_ = nullptr;
    GtkWidget* overlay_ = nullptr;
    GtkWidget* image_ = nullptr;
    GtkWidget* badge_ = nullptr;
    Slot slot_;
    std::function<void()> on_click_;
};

} // namespace realmheart::ui::components
