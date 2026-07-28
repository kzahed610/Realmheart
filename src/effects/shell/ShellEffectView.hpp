#pragma once

#include "effects/core/EffectFrame.hpp"

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define REALMHEART_TYPE_SHELL_EFFECT_VIEW \
    (realmheart_shell_effect_view_get_type())
G_DECLARE_FINAL_TYPE(
    RealmheartShellEffectView,
    realmheart_shell_effect_view,
    REALMHEART,
    SHELL_EFFECT_VIEW,
    GtkWidget
)

GtkWidget* realmheart_shell_effect_view_new(GtkWidget* child);

G_END_DECLS

namespace realmheart::effects::shell {

void set_frame(
    RealmheartShellEffectView* view,
    const EffectFrame& frame
) noexcept;

void set_origin(
    RealmheartShellEffectView* view,
    double normalized_x,
    double normalized_y
) noexcept;

} // namespace realmheart::effects::shell
