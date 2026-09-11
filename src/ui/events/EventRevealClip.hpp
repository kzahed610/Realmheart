#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define REALMHEART_TYPE_EVENT_REVEAL_CLIP \
    (realmheart_event_reveal_clip_get_type())
G_DECLARE_FINAL_TYPE(
    RealmheartEventRevealClip,
    realmheart_event_reveal_clip,
    REALMHEART,
    EVENT_REVEAL_CLIP,
    GtkWidget
)

// Keeps the child at its final allocation and only changes the snapshot clip.
// This is important for layer-shell surfaces: the Wayland surface itself does
// not continuously resize while an event unfurls/furls, avoiding compositor
// shadow/damage trails during the closing animation.
GtkWidget* realmheart_event_reveal_clip_new(
    GtkWidget* child,
    guint opening_duration_ms,
    guint closing_duration_ms
);

void realmheart_event_reveal_clip_set_revealed(
    RealmheartEventRevealClip* self,
    gboolean revealed
);

void realmheart_event_reveal_clip_set_revealed_immediately(
    RealmheartEventRevealClip* self,
    gboolean revealed
);

// Returns the requested target state, including while an animation is active.
gboolean realmheart_event_reveal_clip_get_revealed(
    RealmheartEventRevealClip* self
);

gboolean realmheart_event_reveal_clip_is_concealed(
    RealmheartEventRevealClip* self
);

G_END_DECLS
