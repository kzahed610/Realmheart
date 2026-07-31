# Realmheart FX

Optional Hyprland compositor backend for Realmheart transitions.

The plugin uses Hyprland's visible render-pass pipeline rather than an
`IWindowTransformer`. It temporarily suppresses the real window, draws its
content through the selected registered effect at `RENDER_POST_WINDOWS`, then
restores or releases resources when the transition finishes or any safety check
fails.

The compositor backend currently registers `none` and `void`. Realmheart Void
remains the only compiled visual effect; the registry exists so future shaders
can be added without rewriting the render lifecycle.

## Automatic open and close policy

Automatic lifecycle effects now use a default-allow policy for ordinary
application windows. Any non-empty window class receives Realmheart Void unless
it matches a conservative exclusion rule.

Current class exclusions cover:

```text
Realmheart-owned windows
Hyprlock
Gamescope
steam_app_* game windows
XDG desktop portals
XWaylandVideoBridge
Polkit/authentication agents
Pinentry/askpass prompts
```

Hyprland layer-shell surfaces do not enter the normal-window event path. The
plugin additionally skips transient/dialog windows, fullscreen windows,
override-redirect windows, windows on invisible workspaces, and windows
transitioning while another Realmheart effect is active. A skipped opening
remains normally visible; a skipped close proceeds normally.

An eligible opening window receives only the reconstruction/open half of
Realmheart Void. The animation clock begins when a usable live texture reaches
the render pass, not merely when the window-open event fires. If no texture is
available within the safety timeout, the plugin immediately restores the real
window.

An eligible closing window receives only the consume/close half. Hyprland's
last-frame snapshot is cropped into a plugin-owned texture before the effect
begins. The application can therefore finish unmapping and destroy its live
surface without invalidating the closing animation. The retained texture is
released immediately when the transition completes or is cancelled.

Only one compositor transition is allowed at a time. If several windows open or
close nearly simultaneously, the first eligible transition animates and the
others proceed normally. Concurrent transition support remains a separate
hardening milestone.

## Controls

```sh
hyprctl realmheart-fx status
hyprctl realmheart-fx cancel

hyprctl realmheart-fx auto-open status
hyprctl realmheart-fx auto-open on
hyprctl realmheart-fx auto-open off

hyprctl realmheart-fx auto-close status
hyprctl realmheart-fx auto-close on
hyprctl realmheart-fx auto-close off

hyprctl realmheart-fx test
hyprctl realmheart-fx test void
hyprctl realmheart-fx test none
```

The manual `test` command still plays the complete close-then-open inspection
cycle on the focused window. Unknown effect names fail without hiding or
modifying the focused window. `none` bypasses shader work and safely cancels an
active transition.

Loading the plugin no longer triggers a manual test automatically.

## Diagnostics

```sh
cat /tmp/realmheart-fx.log
```

Automatic lifecycle transitions log whether they were armed, started,
completed, or skipped for a safety reason.

## Licensing

The plugin is GPL-3.0-or-later. Render-pass plumbing is adapted from 
hyprfx and xhos/hyprfx; see `ATTRIBUTION.md` and `LICENSE`.
