#pragma once

#include "ui/components/BaseWidget.hpp"
#include "services/ThemeService.hpp"
#include <gtk/gtk.h>
#include <string>
#include <functional>

namespace realmheart::ui::components {

class ThemeableWidget : public BaseWidget {
public:
    virtual ~ThemeableWidget() = default;
    virtual void apply_theme(const services::Palette& palette) = 0;
};

} // namespace realmheart::ui::components
