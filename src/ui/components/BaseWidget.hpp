#pragma once

#include <gtk/gtk.h>
#include <memory>
#include <string>
#include <functional>
#include "services/ThemeService.hpp"

namespace realmheart::ui::components {

class BaseWidget {
public:
    virtual ~BaseWidget() = default;
    virtual GtkWidget* get_widget() = 0;
    virtual void refresh() {}
};

class ThemeableWidget : public BaseWidget {
public:
    virtual ~ThemeableWidget() {
        if (provider_) g_object_unref(provider_);
    }
    virtual void apply_theme(const services::Palette& palette) = 0;
protected:
    GtkCssProvider* provider_ = nullptr;
};

} // namespace realmheart::ui::components
