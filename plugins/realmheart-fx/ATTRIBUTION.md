# Realmheart FX attribution

The Hyprland render-pass integration in this optional plugin is adapted from:

- ** hyprfx**, commit `d680dabdd2d9362626ecedcad9bd396508163468`
-  hyprfx states that its pass-element skeleton and name originated in **xhos/hyprfx**
- xhos/hyprfx in turn credits **Burn-My-Windows** shader ports

The adapted portions include the custom `IPassElement` strategy, injection at
`RENDER_POST_WINDOWS`, live surface-texture lookup, damage-driven frame loop,
and alpha/no-animation overrides used while the shader draws the window.

Realmheart Void, Realmheart's transition registry, shell-side GTK renderer, and
Realmheart-specific control/lifecycle code remain Realmheart work.

This plugin directory is distributed under **GPL-3.0-or-later**. See `LICENSE`.
