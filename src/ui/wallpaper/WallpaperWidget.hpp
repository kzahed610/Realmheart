#pragma once

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <iostream>
#include <string>
#include <memory>

namespace realmheart::ui::wallpaper {

class WallpaperWidget : public GObject {
public:
    static GType get_type();
    static GtkWidget* create();

    // Method to update the current wallpaper texture
    void set_texture(GdkTexture* texture);

    // GObject boilerplate for C++
    GObject* operator new(size_t size);
    void operator delete(size_t size);
};

} // namespace realmheart::ui::wallpaper
