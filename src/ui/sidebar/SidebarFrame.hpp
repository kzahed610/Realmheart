#pragma once

#include "ui/sidebar/SidebarGeometry.hpp"

#include <gtk/gtk.h>

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
};

} // namespace realmheart::ui::sidebar
