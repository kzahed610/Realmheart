# Realmheart FX

Optional Hyprland compositor backend for Realmheart transitions.

The plugin uses Hyprland's visible render-pass pipeline rather than an
`IWindowTransformer`. It temporarily suppresses the real window, draws its
content through the selected registered effect at `RENDER_POST_WINDOWS`, then
restores or releases resources when the transition finishes or any safety check
fails.

The compositor backend currently registers `none`, `void`, and
`aether-sunder`. Realmheart Void remains the default effect. Aether Sunder is a
second, deliberately simple shader used to prove that the same compositor
lifecycle can compile, select, and play more than one visual effect.

## TOML effect assignments

Window effect assignment is loaded from:

```text
$XDG_CONFIG_HOME/realmheart/window-effects.toml
```

or, when `XDG_CONFIG_HOME` is unset:

```text
~/.config/realmheart/window-effects.toml
```

`REALMHEART_FX_CONFIG=/absolute/path.toml` can override the location. Missing
configuration preserves the built-in policy: Realmheart Void for ordinary
eligible windows and Aether Sunder for Kitty. An invalid startup file also falls
back safely; an invalid manual reload preserves the previous valid policy.

Example:

```toml
[windows]
open = "void"
close = "void"

[[windows.rules]]
class = "kitty"
class_match = "exact"
open = "aether-sunder"
close = "aether-sunder"
```

Rules are evaluated top-to-bottom and matching is case-insensitive. A rule may
match `class`, `title`, or both. `class_match` and `title_match` accept `exact`,
`prefix`, or `contains`. A rule may assign only `open`, only `close`, or both.
The registered effect name `none` disables that direction for matching windows.

The parser intentionally supports this small, strict TOML assignment schema
rather than arbitrary TOML values. Unknown tables, keys, match modes, or effect
names reject the reload with a line-numbered error.

Install the included baseline:

```sh
mkdir -p ~/.config/realmheart
cp plugins/realmheart-fx/window-effects.toml.example \
   ~/.config/realmheart/window-effects.toml
hyprctl realmheart-fx config reload
```

## Automatic open and close policy

Automatic lifecycle effects use a default-allow policy for ordinary application
windows. The TOML defaults and ordered rules select the registered effect after
the conservative safety exclusions are applied.

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
begins. For a tiled close, Realmheart also snapshots the other tiled windows on
the workspace before Hyprland removes the target from the layout. During the
transition it redraws the complete retained old scene at its original geometry,
including the compositor gaps between tiles. Once Void has fully consumed the
closing window, Realmheart restores each surviving live window to its old tile
geometry and reassigns Hyprland's already-calculated final geometry. Hyprland
then performs the genuine tiled resize animation after the closing effect, rather
than Realmheart stretching a frozen screenshot or cutting directly to the final
layout. Every retained texture is released immediately when the transition
completes or is cancelled.

Only one compositor transition is allowed at a time. If several windows open or
close nearly simultaneously, the first eligible transition animates and the
others proceed normally. Concurrent transition support remains a separate
hardening milestone.

## Controls

```sh
hyprctl realmheart-fx status
hyprctl realmheart-fx cancel

hyprctl realmheart-fx config status
hyprctl realmheart-fx config path
hyprctl realmheart-fx config reload

hyprctl realmheart-fx auto-open status
hyprctl realmheart-fx auto-open on
hyprctl realmheart-fx auto-open off

hyprctl realmheart-fx auto-close status
hyprctl realmheart-fx auto-close on
hyprctl realmheart-fx auto-close off

hyprctl realmheart-fx test
hyprctl realmheart-fx test void
hyprctl realmheart-fx test aether-sunder
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
completed, or skipped for a safety reason. Configuration startup and reload
results are logged with the active defaults, rule count, and source path.

## Licensing

The plugin is GPL-3.0-or-later. Render-pass plumbing is adapted from 
hyprfx and xhos/hyprfx; see `ATTRIBUTION.md` and `LICENSE`.
