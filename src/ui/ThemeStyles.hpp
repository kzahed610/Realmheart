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
    struct CssProviderDeleter {
        void operator()(GtkCssProvider* provider) const noexcept {
            if (provider != nullptr) g_object_unref(provider);
        }
    };

    void apply(const services::Palette& palette);

    std::shared_ptr<services::ThemeService> theme_service_;
    services::ThemeService::Subscription subscription_;
    GdkDisplay* display_ = nullptr;
    std::unique_ptr<GtkCssProvider, CssProviderDeleter> provider_;
    std::string component_css_;
};

} // namespace realmheart::ui
