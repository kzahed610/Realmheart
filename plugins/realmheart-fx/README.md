# Realmheart FX

Optional Hyprland compositor backend for Realmheart transitions.

The plugin uses Hyprland's visible render-pass pipeline rather than an
`IWindowTransformer`. It hides the selected real window temporarily, draws its
live surface texture through the selected registered effect at
`RENDER_POST_WINDOWS`, then restores the window after the manual
close-then-open inspection cycle.

The compositor backend currently registers `none` and `void`. Realmheart Void
remains the only compiled visual effect; the registry exists so future shaders
can be added without rewriting the render lifecycle.

## Prototype control

The plugin runs Realmheart Void once against the focused normal window when
loaded. It also registers:

```sh
hyprctl realmheart-fx test
hyprctl realmheart-fx test void
hyprctl realmheart-fx test none
hyprctl realmheart-fx status
hyprctl realmheart-fx cancel
```

Unknown effect names fail without hiding or modifying the focused window.
`none` bypasses shader work and cancels any active manual test safely.

## Licensing

The plugin is GPL-3.0-or-later. Render-pass plumbing is adapted from 
hyprfx and xhos/hyprfx; see `ATTRIBUTION.md` and `LICENSE`.
