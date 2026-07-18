#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define REALMHEART_TYPE_VERTICAL_REVEAL_CLIP \
    (realmheart_vertical_reveal_clip_get_type())
G_DECLARE_FINAL_TYPE(
    RealmheartVerticalRevealClip,
    realmheart_vertical_reveal_clip,
    REALMHEART,
    VERTICAL_REVEAL_CLIP,
    GtkWidget
)

// Keeps the child at its final size and reveals it through a top-anchored
// snapshot clip. Opening begins as a softly appearing centred pill, widens
// into the panel's leading strip, then uncovers the full child downward.
GtkWidget* realmheart_vertical_reveal_clip_new(
    GtkWidget* child,
    guint opening_duration_ms,
    guint closing_duration_ms,
    guint initial_strip_px
);

void realmheart_vertical_reveal_clip_set_revealed(
    RealmheartVerticalRevealClip* self,
    gboolean revealed
);

void realmheart_vertical_reveal_clip_set_revealed_immediately(
    RealmheartVerticalRevealClip* self,
    gboolean revealed
);

gboolean realmheart_vertical_reveal_clip_is_concealed(
    RealmheartVerticalRevealClip* self
);

G_END_DECLS
