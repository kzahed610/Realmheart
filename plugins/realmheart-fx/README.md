# Realmheart FX

Optional Hyprland compositor backend for Realmheart transitions.

The plugin uses Hyprland's visible render-pass pipeline rather than an
`IWindowTransformer`. It temporarily suppresses the real window, draws its
content through the selected registered effect at `RENDER_POST_WINDOWS`, then
restores or releases resources when the transition finishes or any safety check
fails.

The compositor backend discovers effect manifests from the Realmheart `effects/`
directory at plugin startup. `none` remains a built-in bypass effect; Realmheart
Void and Aether Sunder are ordinary manifest-defined effects using the same
renderer and lifecycle machinery.

## Effect manifests

Every compatible effect lives in one direct child directory of `effects/`:

```text
effects/
└── mana-shatter/
    ├── effect.toml
    └── mana-shatter.frag
```

A minimal manifest is:

```toml
[effect]
id = "mana-shatter"
display_name = "Mana Shatter"
shader = "mana-shatter.frag"
open_duration = 0.70
close_duration = 0.75
reversible = true
```

The current renderer defaults the common capabilities to enabled. They may be
made explicit or selectively disabled:

```toml
[capabilities]
source_texture = true
texture_2d = true
external_texture = true
rounded_source = true
```

Effect IDs use lowercase letters, digits, and hyphens. `none` is reserved. The
shader path must remain relative to its effect directory. Duplicate IDs,
missing shaders, malformed manifests, unsafe paths, and unsupported keys abort
plugin startup safely instead of producing a partially compiled registry.

After adding a folder, rebuild and reload the plugin. No C++ registry enum,
registry table, policy code, or compositor lifecycle code needs to change. The
new ID becomes available to `test` and to `window-effects.toml` automatically.

```sh
hyprctl realmheart-fx effects
hyprctl realmheart-fx test mana-shatter
```

The initial manifest contract is intentionally limited to compatible
single-texture fragment shaders using the established Realmheart uniforms. A
future effect requiring extra textures, custom render passes, or unrelated
uniform contracts still requires a renderer extension.

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
configuration preserves the built-in policy: every registered non-none effect
forms the random default pool for eligible windows. An invalid startup file also
falls back safely; an invalid manual reload preserves the previous valid policy.

Randomize every eligible lifecycle event across every registered effect:

```toml
[windows]
open = "@all"
close = "@all"
```

`@all` expands when the config is loaded, so a compatible future manifest joins
the pool automatically after the plugin is rebuilt/reloaded. An explicit array
creates a controlled random pool instead:

```toml
[windows]
open = ["void", "aether-sunder"]
close = ["void", "aether-sunder"]
```

A single quoted effect remains a fixed assignment. Rules support the same three
forms:

```toml
[[windows.rules]]
class = "kitty"
class_match = "exact"
open = ["aether-sunder", "void"]
close = "aether-sunder"
```

Each eligible open or close event chooses independently and uniformly from its
resolved pool; repeated selections are valid random outcomes. Rules are
evaluated top-to-bottom and matching is case-insensitive. A rule may match
`class`, `title`, or both. `class_match` and `title_match` accept `exact`,
`prefix`, or `contains`. A rule may assign only `open`, only `close`, or both.
The registered effect name `none` disables that direction; including `none` in
an array gives the event a chance to run without an effect.

The parser intentionally supports this small, strict TOML assignment schema
rather than arbitrary TOML values. Arrays must be one-line quoted-string arrays.
Empty pools, duplicates, `@all` inside an array, unknown tables, keys, match
modes, or effect names reject the reload with a line-numbered error.

Install the included baseline:

```sh
mkdir -p ~/.config/realmheart
cp plugins/realmheart-fx/window-effects.toml.example \
   ~/.config/realmheart/window-effects.toml
hyprctl realmheart-fx config reload
```

## Automatic open and close policy

Automatic lifecycle effects use a default-allow policy for ordinary application
windows. The TOML defaults and ordered rules resolve a fixed effect or random effect pool
after the conservative safety exclusions are applied.

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

An eligible opening window receives only the reconstruction/open half of its
randomly or explicitly selected effect. The animation clock begins when a usable live texture reaches
the render pass, not merely when the window-open event fires. If no texture is
available within the safety timeout, the plugin immediately restores the real
window.

An eligible closing window receives only the consume/close half of its selected effect. Hyprland's
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
hyprctl realmheart-fx effects

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
completed, or skipped for a safety reason. Plugin startup logs the discovered
manifest count and IDs. Configuration startup and reload results are logged with
the active defaults, rule count, and source path.

## Licensing

The plugin is GPL-3.0-or-later. Render-pass plumbing is adapted from 
hyprfx and xhos/hyprfx; see `ATTRIBUTION.md` and `LICENSE`.
