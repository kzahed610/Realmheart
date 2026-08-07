<p align="center">
  <img
    src="assets/Realmheart-Icons/realmheart-icon.png"
    alt="Realmheart sigil"
    width="256"
  />
</p>

<h1 align="center">Realmheart</h1>

<p align="center">
  <strong>A native, TBATE-inspired desktop shell for Hyprland.</strong>
  <br />
  Built in C++ and GTK 4 for a desktop that refuses to choose between
  <em>performance</em> and <em>presence</em>.
</p>

<p align="center">
  <img alt="C++ 20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus&logoColor=white" />
  <img alt="GTK 4" src="https://img.shields.io/badge/GTK-4-7FE719?style=flat-square&logo=gtk&logoColor=111111" />
  <img alt="Wayland" src="https://img.shields.io/badge/Wayland-native-FFBC00?style=flat-square&logo=wayland&logoColor=111111" />
  <img alt="Hyprland" src="https://img.shields.io/badge/Hyprland-only-58E1FF?style=flat-square" />
  <img alt="Status" src="https://img.shields.io/badge/status-active%20development-C084FC?style=flat-square" />
</p>

---

## What is Realmheart?

Realmheart is a **native desktop shell built specifically for Hyprland**. It
replaces the usual collection of unrelated bars, launchers, sidebars, OSDs,
notification daemons, wallpaper tools, and session menus with one cohesive
system.

The shell is heavily inspired by **The Beginning After the End**, but the theme
is not merely painted over generic widgets. Realmheart treats animation,
silhouette, interaction, and resource usage as parts of the same design.

Its guiding idea is simple:

> Build only the desktop features that are genuinely useful, then make those
> features feel like they belong to the same world.

> [!IMPORTANT]
> Realmheart is currently a personal, actively developed shell targeting a
> **CachyOS/Arch + Hyprland** environment. It is not yet a polished,
> plug-and-play shell for arbitrary distributions or compositor setups.

---

## Highlights

| Surface | What it does |
| --- | --- |
| **Aether Spine** | A sculpted left-side bar with launcher access, media controls, system monitoring, workspace runes, clock, battery, Wi-Fi, notifications, and power controls. |
| **Realmheart Launcher** | Searches applications and actions, ranks frequently used apps, focuses existing windows, evaluates calculations, runs fish commands, browses clipboard history, and searches emoji. |
| **Right Sidebar** | Manages Wi-Fi, Bluetooth, brightness, volume, Night Light, Keep Awake, performance profiles, GameMode, and notification history. |
| **Animated Power Menu** | A fullscreen native control layer over a cinematic TBATE-inspired scene, with lock, suspend, logout, reboot, and power-off actions. |
| **Wallpaper Engine** | Uses either a GTK fallback or a separate Wayland/EGL/OpenGL ES renderer with output-aware cover cropping and animated transitions. |
| **Dynamic Theme System** | Generates and caches a Matugen palette from the active wallpaper, then updates shell surfaces through one shared GTK CSS provider. |
| **Notification System** | Implements a D-Bus notification server, transient toasts, unread state, and a persistent in-session notification history. |
| **Desktop Utilities** | Provides volume and brightness OSDs, screenshots, region capture, OCR-to-clipboard, screen recording, quick notes, and live shell control. |
| **Animation Framework** | Includes layered character composition, expression animation, spring motion, mesh deformation, flow warping, shader playback, and transition timelines. |
| **Realmheart FX** | An optional Hyprland plugin for native compositor-level open and close effects. |

---

## The shell

### Aether Spine

The left rail is Realmheart's persistent control surface. It is intentionally
narrow, asymmetric, and visually attached to the screen edge instead of looking
like a floating rectangle.

It currently includes:

- a Realmheart launcher button;
- compact MPRIS media controls with album art and seek support;
- an on-demand CPU, RAM, GPU, and temperature monitor;
- animated Hyprland workspace runes with window previews;
- a clock and calendar surface;
- battery state and details;
- live Wi-Fi and notification indicators;
- a direct entry point to the native power menu.

Most expensive work is event-driven or performed only while the relevant
popover is visible. Recovery polling is deliberately slow and bounded.

### Realmheart Launcher

The launcher is more than an application list. It combines a spatial pinned-app
**constellation** with a central search and command surface.

It supports:

- desktop application discovery through GIO;
- fuzzy matching across names, descriptions, executables, and desktop IDs;
- usage-aware ranking based on launches and Hyprland activity;
- pinning, unpinning, and persistent constellation placement;
- grouping and focusing already-running Hyprland windows;
- custom shell actions from `~/.config/realmheart/actions/`;
- safe calculator expressions with copy-to-clipboard results;
- explicit fish command execution;
- text and image clipboard history through `cliphist`;
- emoji search and copy;
- keyboard and pointer navigation;
- a wallpaper-backed central composition and command receipt animation.

#### Launcher query language

| Input | Behaviour |
| --- | --- |
| `firefox` | Search installed applications and custom actions. |
| `12 * (7 + 3)` | Evaluate the expression and offer the result for copying. |
| `> btop` | Run `btop` explicitly through fish. `$ btop` works as well. |
| `>clip` | Open clipboard history. |
| `>clip youtube` | Filter clipboard text and image entries. |
| `>emoji fire` | Search emoji by keyword. |
| `>clear` | Clear clipboard history after explicit confirmation. |

> [!NOTE]
> Clipboard mode requires `cliphist` and `wl-clipboard`. Emoji mode reads the
> data source configured by `REALMHEART_EMOJI_DATA`, falling back to
> `~/.config/hypr/hyprland/scripts/fuzzel-emoji.sh`.

### Right Sidebar

The sidebar acts as the shell's control centre. Services are queried and mutated
asynchronously so opening the panel does not block GTK's main loop.

Current controls include:

- Wi-Fi radio, network scanning, connection, and saved-network management;
- Bluetooth power, pairing, connection, disconnection, and device removal;
- volume and brightness sliders;
- Night Light control through `hyprsunset`;
- Keep Awake / idle inhibition;
- GameMode state and toggling;
- power profile selection;
- scrollable notification history;
- an optional layered character presentation with selectable hair modes.

Supported character hair modes are:

```text
static
mesh
mesh-flow
```

### Native Power Menu

Realmheart's power menu is a fullscreen GTK layer-shell surface containing a
native interaction layer over a video-backed scene.

Available actions:

```text
Lock · Suspend · Logout · Restart · Power off
```

The menu supports keyboard and pointer control, animated hover states, delayed
interaction setup, and deliberate two-step confirmation before an action is run.

The current scene asset is available at
[`assets/power-menu/realmheart-power-menu.mp4`](assets/power-menu/realmheart-power-menu.mp4).

### Wallpaper and theming

Wallpaper state is independent from wallpaper rendering:

```text
WallpaperController
├── GtkWallpaperBackend
└── NativeWallpaperBackend
    └── realmheart-wallpaper-renderer
```

The native renderer runs as a separate process and provides:

- one layer-shell background surface per output;
- output-aware image decode sizing;
- cover-style cropping;
- output hotplug and scale handling;
- a 350 ms wallpaper crossfade;
- idle blocking when no transition is active;
- automatic fallback to the GTK renderer when startup or rendering fails.

Once a wallpaper is accepted, Realmheart asks Matugen for a dark palette,
validates the result, caches it, and updates one display-wide GTK CSS provider.

See [`docs/wallpaper-backends.md`](docs/wallpaper-backends.md) for backend details.

### Utilities

Realmheart exposes the following utilities through its persistent shell:

- fullscreen screenshots saved under `Pictures/Screenshots/`;
- region screenshots copied directly to the clipboard;
- region OCR copied to the clipboard;
- screen recordings saved under `Videos/Recordings/`;
- debounced plaintext notes stored with atomic file replacement;
- brightness and volume OSD overlays;
- wallpaper selection and theme regeneration;
- in-place shell restart without losing the selected wallpaper backend.

---

## Architecture

Realmheart keeps UI state, system integration, and rendering concerns separated.

```text
Realmheart
├── core/
│   ├── bounded command execution
│   ├── diagnostics and module registry
│   └── live shell command delivery
├── services/
│   ├── Hyprland workspace and window tracking
│   ├── audio, brightness, battery, Wi-Fi, and Bluetooth
│   ├── media, notifications, launcher, notes, and theming
│   └── wallpaper, utilities, power profiles, and session actions
├── ui/
│   ├── vertical bar and popovers
│   ├── right sidebar
│   ├── launcher and command receipts
│   ├── power menu
│   ├── OSDs, notifications, notes, and wallpaper surfaces
│   └── shared components, assets, and modular CSS
├── animation/
│   ├── character rigging and composition
│   ├── mesh and flow deformation
│   └── spring-based motion
├── effects/
│   ├── shader registry and playback
│   └── shell transition rendering
├── wallpaper-native/
│   └── standalone Wayland/EGL/GLES wallpaper renderer
└── plugins/realmheart-fx/
    └── optional Hyprland compositor plugin
```

### Design principles

1. **Native first** — C++20, GTK 4, Wayland, EGL, and compositor IPC instead of a browser runtime.
2. **Lazy surfaces** — heavyweight overlays and services are initialized only when needed.
3. **Bounded work** — external commands have deadlines and output limits.
4. **Event-driven state** — Hyprland, media, notifications, and UI updates avoid aggressive polling.
5. **One visual system** — shared palette roles, reusable SVG geometry, modular CSS, and consistent motion.
6. **Failure isolation** — optional native renderers and effects must fail without taking down the shell.

---

## Requirements

### Build dependencies

- CMake 3.25 or newer;
- Ninja;
- a C++20 compiler;
- GTK 4.12 or newer;
- GTK4 Layer Shell;
- GIO / GLib;
- GdkPixbuf;
- libjpeg;
- libepoxy;
- pthreads;
- GoogleTest when building the test suite.

On Arch Linux or CachyOS, the core build stack is typically:

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf gtest \
  gtk4 gtk4-layer-shell glib2 gdk-pixbuf2 \
  libjpeg-turbo libepoxy
```

### Core runtime tools

| Command | Used for |
| --- | --- |
| `hyprctl` | Hyprland workspaces, windows, events, focus, and logout. |
| `brightnessctl` | Backlight state and mutation. |
| `wpctl` | PipeWire volume and mute control. |
| `nmcli` | Wi-Fi state and network management. |
| `bluetoothctl` | Bluetooth state and device management. |
| `powerprofilesctl` | Power Saver, Balanced, and Performance profiles. |
| `matugen` | Wallpaper-derived theme generation. |

Run `realmheart --doctor` after building to see which tools Realmheart can find.

### Optional runtime tools

| Command | Enables |
| --- | --- |
| `hyprlock` | Lock action. |
| `hypridle` | Idle/session integration. |
| `hyprsunset` | Night Light. |
| `grim` + `slurp` | Screenshot and region capture. |
| `wl-copy` + `wl-paste` | Clipboard integration. |
| `cliphist` | Clipboard history, image previews, deletion, and wipe. |
| `tesseract` | Region OCR. |
| `wf-recorder` | Screen recording. |
| `curl` | Remote MPRIS album-art caching. |
| `systemd-run` | Isolated application and command launch scopes when Realmheart itself runs as a systemd user unit. |

### Native wallpaper renderer

The native wallpaper backend is built automatically when CMake finds:

- Wayland client and Wayland EGL;
- EGL and OpenGL ES 2;
- `wayland-scanner`;
- `wayland-protocols`;
- `wlr-protocols`.

On Arch Linux or CachyOS:

```bash
sudo pacman -S --needed wayland wayland-protocols wlr-protocols mesa
```

---

## Build

```bash
# From the repository root
cmake -S . -B build-hybrid -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DREALMHEART_ENABLE_NATIVE_WALLPAPER=ON \
  -DBUILD_TESTING=ON

cmake --build build-hybrid -j"$(nproc)"
ctest --test-dir build-hybrid --output-on-failure
```

The main outputs are:

```text
build-hybrid/realmheart
build-hybrid/realmheart-wallpaper-renderer   # when native dependencies exist
build-hybrid/realmheart-gl-probe
build-hybrid/realmheart-fx.so                # when compatible Hyprland headers exist
```

Disable optional components when required:

```bash
cmake -S . -B build -G Ninja \
  -DREALMHEART_ENABLE_NATIVE_WALLPAPER=OFF \
  -DREALMHEART_BUILD_HYPRLAND_PLUGIN=OFF \
  -DBUILD_TESTING=OFF
```

Install the shell, assets, styles, effects, and available optional targets:

```bash
sudo cmake --install build-hybrid
```

---

## Run

First inspect the environment:

```bash
./build-hybrid/realmheart --doctor
```

Start the complete shell with the native wallpaper backend:

```bash
./build-hybrid/realmheart --shell --wallpaper-backend native
```

Use the GTK wallpaper fallback instead:

```bash
./build-hybrid/realmheart --shell --wallpaper-backend gtk
```

The default can also be selected through the environment:

```bash
export REALMHEART_WALLPAPER_BACKEND=native
./build-hybrid/realmheart --shell
```

---

## Hyprland integration

Use an absolute path for development builds:

```ini
# Start Realmheart with Hyprland.
exec-once = /absolute/path/to/Realmheart/build-hybrid/realmheart --shell --wallpaper-backend native

# Core surfaces.
bind = SUPER, SPACE, exec, /absolute/path/to/Realmheart/build-hybrid/realmheart --command launch-launcher
bind = SUPER, V,     exec, /absolute/path/to/Realmheart/build-hybrid/realmheart --command launch-launcher-query ">clip "
bind = SUPER, PERIOD,exec, /absolute/path/to/Realmheart/build-hybrid/realmheart --command launch-launcher-query ">emoji "
bind = SUPER, G,     exec, /absolute/path/to/Realmheart/build-hybrid/realmheart --command toggle-notes
bind = SUPER, S,     exec, /absolute/path/to/Realmheart/build-hybrid/realmheart --command sidebar-right-toggle
bind = SUPER SHIFT, E, exec, /absolute/path/to/Realmheart/build-hybrid/realmheart --command logout-menu

# Development reload.
bind = SUPER, R, exec, /absolute/path/to/Realmheart/build-hybrid/realmheart --command restart
```

Installed builds can invoke `realmheart` directly from `PATH`.

---

## Live shell control

A running shell is controlled without restarting the session:

```bash
realmheart --command <name> [argument]
```

### Surfaces and overlays

```bash
realmheart --command launch-launcher
realmheart --command launch-launcher-query ">clip "
realmheart --command workspace-overview-toggle
realmheart --command sidebar-right-toggle
realmheart --command bar-toggle
realmheart --command toggle-notes
realmheart --command logout-menu
realmheart --command osd-volume
realmheart --command osd-brightness
```

### Workspace morph diagnostics

The rune-to-realm transition is transparent outside its geometry-owned reveal
clips and falls back to the native geometry animation if its optional shader
host fails. Opt-in timing and memory diagnostics are available without adding
idle work:

```fish
systemctl --user set-environment REALMHEART_WORKSPACE_MORPH_DIAGNOSTICS=1
systemctl --user restart realmheart.service
./tools/workspace-morph-profile.fish --cycles 30 --rapid 20
```

Each completed transition reports its endpoint, elapsed time, frame count,
worst frame interval, reversal count, shader state, peak transition texture
bytes, retained transition bytes, and process RSS delta. Disable the logs with:

```fish
systemctl --user unset-environment REALMHEART_WORKSPACE_MORPH_DIAGNOSTICS
systemctl --user restart realmheart.service
```

The workspace overview is also toggled by right-clicking any workspace rune in
the Aether Spine. It docks beside the bar and presents a four-workspace viewport
that follows Hyprland beyond workspace 4. For example, activating workspace 5
shifts the visible range to workspaces 2–5. Fire, Water, Wind, and Earth cycle
across later workspaces, so workspace 5 returns to Bairon and the Fire realm.

The overview uses live client class/title data and cached application icons.
Client open, close, move, and title changes refresh the cards. Clicking a card
focuses that exact window; dragging a card into another visible realm moves the
client there. A held drag survives workspace scrolling, allowing `SUPER` plus
the configured workspace wheel bind to reveal a later destination before drop.

Use `Up`/`Down` or `K`/`J` to select a realm, `Left`/`Right` or `H`/`L` to
select a window, and `Enter` to activate it. Number keys `1`–`4` select the four
currently visible realm slots, while `Escape` or clicking unused background
closes the overview.

### Character controls

```bash
realmheart --command character-toggle
realmheart --command character-hair-mode static
realmheart --command character-hair-mode mesh
realmheart --command character-hair-mode mesh-flow
```

### Wallpaper and theme

```bash
realmheart --command set-wallpaper
realmheart --command set-wallpaper-path /absolute/path/to/wallpaper.png
realmheart --command set-wallpaper-backend native
realmheart --command set-wallpaper-backend gtk
realmheart --command generate-theme
```

### Capture and session actions

```bash
realmheart --command screenshot-full
realmheart --command screenshot-area
realmheart --command extract-ocr
realmheart --command start-recording
realmheart --command stop-recording
realmheart --command lock-session
realmheart --command restart
realmheart --command quit
```

---

## Diagnostics and isolated surface testing

```bash
# Full dependency and live state report.
./build-hybrid/realmheart --doctor

# Confirm compiled modules.
./build-hybrid/realmheart --list-modules

# Inspect Hyprland workspace state.
./build-hybrid/realmheart --workspace-status

# Inspect sidebar services.
./build-hybrid/realmheart --right-sidebar-status

# Launch isolated test surfaces.
./build-hybrid/realmheart --bar --timeout 5
./build-hybrid/realmheart --sidebar --timeout 5
./build-hybrid/realmheart --test-layer --timeout 5
```

Compare wallpaper backend memory using total proportional set size rather than a
single process's RSS:

```bash
smem -P 'realmheart|realmheart-wallpaper-renderer' \
  -c 'pid name rss pss uss'
```

---

## Configuration and state

Realmheart follows XDG directories where possible.

| Path | Purpose |
| --- | --- |
| `~/.config/realmheart/actions/` | Custom launcher shell actions. |
| `~/.config/realmheart/launcher-pins.txt` | Pinned launcher application identities. |
| `~/.config/realmheart/notes.txt` | Quick-note contents. |
| `~/.config/realmheart/window-effects.toml` | Optional Realmheart FX rules. |
| `~/.local/state/realmheart/launcher-history.tsv` | Usage and activity ranking history. |
| `~/.local/state/realmheart/launcher-layout.tsv` | Spatial launcher constellation positions. |
| `~/.local/state/realmheart/theme-palette.tsv` | Validated cached theme palette. |
| `$XDG_RUNTIME_DIR/realmheart/` | Temporary screen-recording process state. |

Useful environment overrides:

| Variable | Purpose |
| --- | --- |
| `REALMHEART_WALLPAPER_BACKEND` | Default wallpaper backend: `gtk` or `native`. |
| `REALMHEART_WALLPAPER_RENDERER` | Explicit native renderer executable. |
| `REALMHEART_STYLE_DIR` | Development override for modular GTK CSS. |
| `REALMHEART_EFFECT_DIR` | Override shader/effect asset root. |
| `REALMHEART_THEME_CACHE` | Override palette cache file. |
| `REALMHEART_EMOJI_DATA` | Override launcher emoji data source. |
| `REALMHEART_CHARACTER_DEBUG` | Enable character compositor diagnostics. |
| `REALMHEART_WORKSPACE_MORPH_DIAGNOSTICS` | Log workspace morph timing, reversal, shader, texture, and RSS diagnostics. |

---

## Realmheart FX

`plugins/realmheart-fx/` contains an optional Hyprland plugin that renders
compositor-native window transitions using Realmheart's shader manifests.

Current effects include:

```text
void
aether-sunder
```

The plugin is **ABI-coupled to Hyprland** and must be rebuilt whenever Hyprland's
plugin ABI changes. Do not keep using an older `.so` after a compositor update.

See [`plugins/realmheart-fx/README.md`](plugins/realmheart-fx/README.md) for
building, loading, rule configuration, safety exclusions, diagnostics, and
controls.

---

## Tests

Realmheart contains focused tests for core services, launcher behaviour,
wallpaper backends, shell control, notifications, character animation, shader
loading, power-menu behaviour, Hyprland integration models, and UI foundations.

Run everything:

```bash
ctest --test-dir build-hybrid --output-on-failure
```

For a clean verification pass:

```bash
rm -rf build-hybrid
cmake -S . -B build-hybrid -G Ninja \
  -DREALMHEART_ENABLE_NATIVE_WALLPAPER=ON \
  -DBUILD_TESTING=ON
cmake --build build-hybrid -j"$(nproc)"
ctest --test-dir build-hybrid --output-on-failure
```

---

## Project status

Realmheart is already used as a functioning shell, but it remains an
**opinionated personal project under heavy development**.

### Working now

- [x] Native GTK4 shell lifecycle and layer-shell surfaces
- [x] Aether Spine vertical bar
- [x] Hyprland workspace and application tracking
- [x] Launcher, calculator, commands, actions, clipboard, and emoji
- [x] Right sidebar and live system controls
- [x] D-Bus notifications, history, and toasts
- [x] GTK and native wallpaper backends
- [x] Matugen palette generation and cache
- [x] Notes, OSDs, screenshots, OCR, and recording
- [x] Animated fullscreen power menu
- [x] Character animation and shader infrastructure
- [x] Optional Realmheart FX Hyprland plugin
- [x] Automated service and behaviour tests

### Still evolving

- [ ] A reproducible installation and first-run setup flow
- [ ] Distribution-neutral dependency handling
- [ ] User-facing configuration instead of source-level tuning
- [ ] Broader multi-monitor and hardware validation
- [ ] More complete documentation for visual asset authoring
- [ ] Stable releases, migration notes, and compatibility guarantees
- [ ] A proper screenshot and video showcase

---

## Scope

Realmheart is intentionally **not** trying to become:

- a complete desktop environment;
- a compositor replacement;
- a universal shell for every Wayland compositor;
- a drop-in clone of Quickshell, AGS, Waybar, or any existing shell;
- a generic framework that sacrifices identity for configurability.

It is a focused Hyprland shell built around one desktop, one design language,
and one very unreasonable standard for aesthetic integrity.

---

## Attribution and licensing

Realmheart is an unofficial fan-made project inspired by **The Beginning After
the End** and is not affiliated with its creators, publishers, or rights
holders. Character and story references belong to their respective owners.

The optional `realmheart-fx` plugin has its own GPL-3.0-or-later license and
attribution files under `plugins/realmheart-fx/`.

The repository root does not currently declare a project-wide software license.
Until one is added, do not assume permission to redistribute or reuse the rest
of the source or bundled assets.

---

<p align="center">
  <strong>Realmheart</strong><br />
  Native where it matters. Excessive where it counts.
</p>
