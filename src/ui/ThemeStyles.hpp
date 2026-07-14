#pragma once

#include "services/ThemeService.hpp"

#include <gtk/gtk.h>
#include <memory>
#include <string>

namespace realmheart::ui {

// Owns one display-wide GTK CSS provider. Widgets only expose stable CSS
// classes; palette changes replace this provider's stylesheet in one place.
class ThemeStyles {
public:
    explicit ThemeStyles(std::shared_ptr<services::ThemeService> theme_service);
    ~ThemeStyles();

    ThemeStyles(const ThemeStyles&) = delete;
    ThemeStyles& operator=(const ThemeStyles&) = delete;

    static std::string build_css(const services::Palette& palette);

private:
    void apply(const services::Palette& palette);

    std::shared_ptr<services::ThemeService> theme_service_;
    services::ThemeService::Subscription subscription_;
    GdkDisplay* display_ = nullptr;
    GtkCssProvider* provider_ = nullptr;
    std::string component_css_;
};

} // namespace realmheart::ui
