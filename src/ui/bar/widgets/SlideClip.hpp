#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define REALMHEART_TYPE_SLIDE_CLIP (realmheart_slide_clip_get_type())
G_DECLARE_FINAL_TYPE(
    RealmheartSlideClip,
    realmheart_slide_clip,
    REALMHEART,
    SLIDE_CLIP,
    GtkWidget
)

// A full-size clipping viewport for horizontal panel reveals. The child keeps
// its final allocation for every frame; only the snapshot clip advances from
// left to right. This avoids the geometry compression produced by GtkRevealer.
GtkWidget* realmheart_slide_clip_new(GtkWidget* child, guint duration_ms);

// Opt into a taskbar-anchored reveal. Instead of leading with the child's far
// right edge, the viewport grows from x = 0 while the full-size child travels
// only `travel_px`. This is useful for shells whose decorative right edge must
// not be the first visible animation frame. A value of 0 keeps the standard
// full-width translation used by the system-stats panel.
void realmheart_slide_clip_set_leading_edge_reveal(
    RealmheartSlideClip* self,
    guint travel_px
);

void realmheart_slide_clip_set_revealed(
    RealmheartSlideClip* self,
    gboolean revealed
);

void realmheart_slide_clip_set_revealed_immediately(
    RealmheartSlideClip* self,
    gboolean revealed
);

gboolean realmheart_slide_clip_is_concealed(RealmheartSlideClip* self);

G_END_DECLS
