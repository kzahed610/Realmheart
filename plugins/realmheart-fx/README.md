# Realmheart FX

Optional Hyprland compositor backend for Realmheart transitions.

This prototype uses Hyprland's visible render-pass pipeline rather than an
`IWindowTransformer`. It hides the selected real window temporarily, draws its
live surface texture through Realmheart Void at `RENDER_POST_WINDOWS`, then
restores the window after a consume/reconstruct cycle.

## Prototype control

The plugin runs once against the focused normal window when loaded. It also
registers:

```sh
hyprctl realmheart-fx test
hyprctl realmheart-fx status
hyprctl realmheart-fx cancel
```

## Licensing

The plugin is GPL-3.0-or-later. Render-pass plumbing is adapted from 
hyprfx and xhos/hyprfx; see `ATTRIBUTION.md` and `LICENSE`.
