#include "ui/wallpaper/WallpaperWidget.hpp"
#include <gtk/gtk.h>

namespace realmheart::ui::wallpaper {

// We'll implement a simple GtkDrawingArea wrapper for now to avoid 
// the complexity of full GObject subclassing in C++ without GObject-introspection.
// This gives us the lowest possible RAM overhead by using a raw Cairo surface.

GtkWidget* WallpaperWidget::create() {
    GtkWidget* drawing_area = gtk_drawing_area_new();
    
    // Set the draw function
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), 
        [](GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
            GdkTexture* texture = static_cast<GdkTexture*>(data);
            if (!texture) return;

            // Calculate scaling to fill the area (Cover)
            double tex_w = gdk_texture_get_width(texture);
            double tex_h = gdk_texture_get_height(texture);
            double scale = std::max(width / tex_w, height / tex_h);
            
            double x = (width - tex_w * scale) / 2.0;
            double y = (height - tex_h * scale) / 2.0;

            gdk_cairo_set_source(cr, texture);
            cairo_scale(cr, scale, scale);
            cairo_translate(cr, x, y);
            cairo_paint(cr);
        }, 
        nullptr // We will update this via a wrapper
    );

    return drawing_area;
}

} // namespace realmheart::ui::wallpaper
