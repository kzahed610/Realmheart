# Realmheart isolated display-tier testing

`isolated_hyprland.py` turns the compatibility plan's manual 1080p / 1440p / 4K
visual gate into a repeatable local harness.

It never reconfigures the physical monitor. The target resolution exists only on
an Aquamarine `headless` output inside a separate Hyprland instance.

The harness has two modes:

- **interactive** — full Realmheart shell in a scaled, mouse/keyboard-capable VNC
  viewer. Use this for actual visual acceptance and clicking around the shell;
- **capture** — deterministic screenshots for quick regression comparisons.

## Isolation model

The harness:

1. snapshots the host Hyprland workspace, active window, and monitor topology;
2. starts a separate nested Hyprland instance from a generated minimal Lua config;
3. isolates that instance's D-Bus and XDG state;
4. prevents the nested compositor from exporting systemd/D-Bus activation vars;
5. creates one Aquamarine `headless` output at the requested logical resolution;
6. disables the nested Wayland-window output inside the test compositor so
   Realmheart has exactly one assigned logical monitor;
7. launches Realmheart inside that compositor;
8. either exposes that headless monitor through localhost-only `wayvnc`, or
   captures it with `grim`;
9. tears down the nested compositor and restores/verifies host state.

The generated compositor config uses current Hyprland Lua syntax. Runtime IPC
has a Lua-dispatch path plus a legacy fallback so host restoration does not
break when the user's main config has already migrated.

## Requirements

Base capture mode:

- a running host Hyprland Wayland session;
- `Hyprland` / `hyprctl`;
- `dbus-run-session`;
- `grim`;
- a built `build-hybrid/realmheart`.

Interactive mode additionally needs:

- `wayvnc`;
- `gtk-vnc`;
- `python-gobject`.

On Arch/CachyOS:

```bash
sudo pacman -S --needed wayvnc gtk-vnc python-gobject
```

## Interactive visual testing

This is the preferred manual compatibility gate:

```bash
python tools/display-tests/isolated_hyprland.py 1440p --interactive
python tools/display-tests/isolated_hyprland.py 4k --interactive

# Regression sanity check after any high-tier tuning:
python tools/display-tests/isolated_hyprland.py 1080p --interactive
```

A normal host window opens with the entire remote framebuffer scaled to fit.
The framebuffer itself remains **2560x1440 / 3840x2160 at logical scale 1**;
resizing the viewer never changes the test monitor geometry.

Click the viewer once when you want its keyboard/mouse grab. Convenience binds
inside the isolated compositor are:

```text
Super+S       right sidebar
Super+O       workspace overview
Super+N       notes
Super+M       wallpaper selector
Super+Space   launcher
Super+Shift+Q end the isolated compositor
```

Closing the viewer window also returns control to the harness, which tears the
session down and restores/verifies host state.

## Screenshot mode

```bash
# Fast single-surface checks.
python tools/display-tests/isolated_hyprland.py 1440p --surface bar
python tools/display-tests/isolated_hyprland.py 1440p --surface sidebar

# Capture bar, sidebar, Workspace Overview, and Notes.
python tools/display-tests/isolated_hyprland.py 4k --surface all
```

Screenshots and compositor/VNC logs are written below
`/tmp/realmheart-display-tests/<timestamp>-<tier>/` unless `--output-dir` is
provided.

The generated XDG/config state lives in its own temporary directory. It is
removed after a clean run. If the harness reports host-restoration warnings, or
`--keep-session` is used, that state directory is retained for inspection.

## Visual gate

For 1440p and 4K, inspect at minimum:

- Aether Spine width, icon size, workspace-rune silhouette, and vertical rhythm;
- sidebar frame/content balance and Tessia hand/hair bounds;
- notification viewport proportions;
- Notes size;
- Workspace Overview tier assets.

Human visual approval remains the final gate, and every high-tier adjustment
gets another 1080p regression pass.
