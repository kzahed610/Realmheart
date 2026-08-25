# Realmheart FX attribution

The Hyprland render-pass integration in this optional plugin is adapted from:

- **hyprfx** (Selene), commit `d680dabdd2d9362626ecedcad9bd396508163468`
- **sandwichfarm/hyprfx**, commit `b60899548fae2317ab5dd47f6ec524a2cd316c66`
- Both implementations trace their pass-element approach to **xhos/hyprfx**
- xhos/hyprfx in turn credits **Burn-My-Windows** shader ports

The adapted portions include the custom `IPassElement` strategy, injection at
`RENDER_POST_WINDOWS`, live target-texture lookup, damage-driven frame loop,
alpha/no-animation overrides, and the target-only lifecycle principle: opening
tracks the live target while closing retains only the disappearing target and
leaves the rest of the compositor scene untouched.

Realmheart Void, Realmheart's transition registry, shell-side GTK renderer, and
Realmheart-specific control/lifecycle code remain Realmheart work.

This plugin directory is distributed under **GPL-3.0-or-later**. See `LICENSE`.
