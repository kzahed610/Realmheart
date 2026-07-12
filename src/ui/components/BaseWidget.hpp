#pragma once

#include <gtk/gtk.h>

namespace realmheart::ui::components {

class BaseWidget {
public:
    virtual ~BaseWidget() = default;
    virtual GtkWidget* get_widget() = 0;
    virtual void refresh() {}
};

} // namespace realmheart::ui::components
