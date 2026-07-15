#pragma once

#include <gtk/gtk.h>
#include <string>

namespace realmheart::ui::bar::widgets {

struct ThemedSvgRenderState;

// Renders the original Realmheart SVG geometry after resolving its semantic
// color tokens. This deliberately avoids GTK symbolic-icon flattening, which
// loses the distinction between hollow strokes, solid fills, and accent marks.
class ThemedSvgIcon {
public:
    explicit ThemedSvgIcon(std::string relative_path = {}, int pixels = 24);
    ~ThemedSvgIcon() = default;

    ThemedSvgIcon(const ThemedSvgIcon&) = delete;
    ThemedSvgIcon& operator=(const ThemedSvgIcon&) = delete;

    GtkWidget* widget() const { return widget_; }

    bool set_icon(std::string relative_path);
    void set_size(int pixels);
    void add_css_class(const char* css_class);

private:
    GtkWidget* widget_ = nullptr;
    ThemedSvgRenderState* state_ = nullptr; // owned by widget_ through g_object data
};

} // namespace realmheart::ui::bar::widgets
