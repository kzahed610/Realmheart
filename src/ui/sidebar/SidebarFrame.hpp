#pragma once

#include <gtk/gtk.h>

namespace realmheart::ui::sidebar {

// Geometry shared by the layer-shell surface, decorative frame, current
// controls, and the future character composition.
//
// The visible frame is 486 px wide. The additional left gutter belongs to
// the transparent surface, so Tessia can later extend beyond the metal shell
// without being clipped by the Wayland surface boundary. Because the input
// region follows only the shell silhouette, the unused gutter stays
// click-through until character interaction is intentionally added.
struct SidebarFrameLayout {
    int frame_width = 486;
    int character_gutter_width = 128;

    int content_inset_start = 76;
    int content_inset_end = 42;
    int content_inset_top = 38;
    int content_inset_bottom = 38;

    [[nodiscard]] constexpr int surface_width() const noexcept {
        return frame_width + character_gutter_width;
    }

    [[nodiscard]] constexpr int frame_origin_x() const noexcept {
        return character_gutter_width;
    }
};

inline constexpr SidebarFrameLayout kDefaultSidebarFrameLayout{};

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
