#pragma once

#include "ui/sidebar/SidebarGeometry.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <vector>

namespace realmheart::ui::sidebar {

// Decorative, non-rectangular shell for the right sidebar.
//
// The existing controls remain ordinary GTK widgets inside a safe rectangular
// inset. Empty back/front art layers are deliberately part of the composition
// now, allowing Tessia's body and hair to sit behind the shell while hands or
// selected strands can later pass in front without rewriting frame geometry.
class SidebarFrame {
public:
    explicit SidebarFrame(
        GtkWindow* window,
        SidebarFrameLayout layout = kDefaultSidebarFrameLayout
    );

    SidebarFrame(const SidebarFrame&) = delete;
    SidebarFrame& operator=(const SidebarFrame&) = delete;

    GtkWidget* widget() const { return root_; }
    void set_child(GtkWidget* child);
    void set_layout(SidebarFrameLayout layout);

    // Toggles the window's input region between the real sidebar silhouette
    // (interactive) and an empty region (fully click-through). Used by the
    // keep-mapped-when-idle optimization: the surface stays mapped at opacity
    // 0 between opens so the next open skips the map round-trip, but while
    // idle it must not intercept pointer input.
    void set_input_region_enabled(bool enabled);

    // Static-layer raster cache. The nine cairo layers never change while the
    // panel is open, but GTK repaints all of them every animation frame (41-95
    // ms of CPU). Rasterizing them once into a GdkMemoryTexture and showing a
    // single GtkPicture turns that into one textured quad per frame.
    void ensure_static_cache();
    void invalidate_static_cache();

    // These full-surface GtkFixed layers are intentionally empty today. Future
    // character renderers may place one or several animated widgets in them.
    GtkWidget* back_art_layer() const { return back_art_layer_; }
    GtkWidget* front_art_layer() const { return front_art_layer_; }
    const SidebarFrameLayout& layout() const { return layout_; }

private:
    SidebarFrameLayout layout_{};
    GtkWidget* root_ = nullptr;
    GtkWidget* content_holder_ = nullptr;
    GtkWidget* content_ = nullptr;
    GtkWidget* back_art_layer_ = nullptr;
    GtkWidget* front_art_layer_ = nullptr;

    struct StaticLayer {
        GtkWidget* widget = nullptr;
        GtkDrawingAreaDrawFunc draw = nullptr;
        gpointer state = nullptr;
        cairo_surface_t* cache = nullptr;
    };
    std::vector<std::unique_ptr<StaticLayer>> static_layers_;
    bool static_cache_valid_ = false;
    GtkWidget* cached_picture_ = nullptr;
};

} // namespace realmheart::ui::sidebar
