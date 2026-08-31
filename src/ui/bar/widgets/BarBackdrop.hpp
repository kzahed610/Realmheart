#pragma once

#include <gtk/gtk.h>

namespace realmheart::ui::bar::widgets {

// Draws the non-rectangular taskbar shell. The rail stays narrow through the
// middle of the screen, while the top and bottom caps extend into the window
// corner area so the bar visually "hugs" rounded application windows.
//
// Colour remains CSS-driven; only the silhouette geometry lives here because
// GTK CSS border-radius can only carve inward inside a rectangular box.
class BarBackdrop {
public:
    BarBackdrop(GtkWindow* window, int rail_width, int visual_width, int curve_height);
    GtkWidget* widget() const { return widget_; }

    // Temporarily omit the top portion of the gold contour while an attached
    // layer-surface panel covers it. Passing 0 restores the complete contour.
    // The rail fill and input region are deliberately unaffected.
    void set_top_contour_occlusion(int bottom_y);
    void set_geometry(int rail_width, int visual_width, int curve_height);

private:
    GtkWidget* widget_ = nullptr;
    GtkWidget* contour_ = nullptr;
};

} // namespace realmheart::ui::bar::widgets
