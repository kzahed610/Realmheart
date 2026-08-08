# Realmheart FX

Optional Hyprland compositor backend for Realmheart transitions.

The plugin uses Hyprland's visible render-pass pipeline rather than an
`IWindowTransformer`. It temporarily suppresses the real window, draws its
content through the selected registered effect at `RENDER_POST_WINDOWS`, then
restores or releases resources when the transition finishes or any safety check
fails.

The compositor backend discovers effect manifests from the Realmheart `effects/windows/`
directory at plugin startup. `none` remains a built-in bypass effect; Realmheart
Void and Aether Sunder are ordinary manifest-defined effects using the same
renderer and lifecycle machinery.

## Effect manifests

Every compatible effect lives in one direct child directory of
`effects/windows/`:

```text
effects/
└── windows/
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

> **All-ordinary-windows build:** `0.10.10-all-ordinary-windows`
> retains the disappearing window and every surviving tiled window as independent
> plugin-owned 2D textures before Hyprland sends the survivors their enlarged
> post-close buffers. The resized live survivors are hidden while their frozen
> pre-close pixels are replayed at the old boxes. Native reflow is revealed only
> after the close effect completes. The abandoned custom `IFadeout` and
> timer-driven rendering branch remains removed; drawing still occurs only
> through Hyprland's normal render pass.

Automatic lifecycle effects use a default-allow policy for ordinary application
windows. The TOML defaults and ordered rules resolve a fixed effect or random
effect pool after the conservative safety exclusions are applied.

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

Hyprland layer-shell surfaces do not enter the normal-window event path. Ordinary
application toplevels, including Swappy, receive the configured Realmheart
transitions. Applications with client-driven late geometry changes may visibly
resize during their effect; users can opt individual classes out with a TOML rule
assigning `open = "none"` and `close = "none"`. The plugin additionally skips
transient/dialog windows, fullscreen windows,
override-redirect windows, windows on invisible workspaces, and windows
transitioning while another Realmheart effect is active. A skipped opening
remains normally visible; a skipped close proceeds normally.

An eligible opening window receives only the reconstruction/open half of its
selected effect. The lifecycle now follows Realmheart's target-only design: the shell suppresses the native target, waits only until
the client has a usable coherent surface, and then samples that live surface texture
and the current Hyprland render box on every frame. It does not freeze a geometry
sample or hand a retained opening snapshot back to the live client. Late client
configures therefore move or resize the effect and the real target together,
and the terminal shader frame is already the same live texture and box that
Hyprland reveals next.

Realmheart sets a temporary `noAnim` override and finishes the already-armed
Hyprland window animation when the opening event is accepted. The shader still
receives the current compositor corner radius every frame, so opaque/RGBX
buffers retain the same rounded silhouette while the native target is hidden.
If a usable source never arrives within the safety timeout, Realmheart removes
its overrides and reveals the window normally.

An eligible closing window receives only the consume/close half of its selected
effect. During the unmap signal Realmheart copies the target's current
`GL_TEXTURE_2D` surface into plugin-owned storage before that surface can be
released or reused. It does not use `makeSnapshotFB()` for closing because that
API can reflect the already-reflowed compositor scene rather than the
disappearing target.

For tiled closes, Realmheart copies each surviving tiled window's current
surface before the layout reflow can replace it with a differently sized client
buffer. At `RENDER_PRE_WINDOWS` it stores Hyprland's newly calculated final
goals, holds the survivors at their old boxes, and alpha-hides the resized live
clients. At `RENDER_POST_WINDOWS` the frozen survivor frames are replayed at
their old rounded boxes beneath the disappearing target's effect. When the
effect ends, the temporary hides are removed, the saved goals are reassigned,
and Hyprland performs the real tiled reflow. Floating windows do not use this
frozen-survivor path.

The compositor lifecycle keeps these invariants:

```text
the close source belongs only to the disappearing target
survivor pixels and layout remain visually frozen during the close effect
resized live client buffers cannot leak through the retained scene
Hyprland owns the eventual reflow animation
```

If the closing client exposes only an external texture that cannot be copied
safely by this probe, Realmheart skips that close and leaves it to Hyprland.

Only one compositor transition is allowed at a time. If several windows open or
close nearly simultaneously, the first eligible transition animates and the
others proceed normally. Concurrent transition support remains a separate
hardening milestone.


## Hyprland ABI compatibility

Realmheart FX is compiled against Hyprland's internal plugin ABI and therefore
must be rebuilt after a Hyprland update. The 0.56 port uses the public geometric
accessors and animation handles plus `Fullscreen::controller()` for fullscreen
state. Closing retains a strong reference to Hyprland's existing per-window rendered
framebuffer texture; the backend never captures or reconstructs the workspace. Plugin startup still rejects a runtime
compositor hash that differs from the headers used at build time.

Do not reuse an older installed `realmheart-fx.so` after upgrading Hyprland.
Build first, load through a fresh unique path, run the lifecycle regressions,
and only then replace the canonical installed binary.

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
# Closing starts disabled; enable it only for a controlled regression test.
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
the active defaults, rule count, and source path. Lifecycle logs identify
whether a transition uses a `live-target` opening source or a
`retained-target-snapshot` closing source, together with its current box and
corner radius. No log entry should mention frozen windows, backdrop masks, or
native reflow restarts in the target-only backend.

## Licensing

The plugin is GPL-3.0-or-later. Upstream provenance for the render-pass and
target-only lifecycle work is consolidated in `ATTRIBUTION.md`; see also
`LICENSE`.
