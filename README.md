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

Realmheart is a desktop shell for Hyprland that replaces your separate bar,
launcher, sidebar, notification daemon, wallpaper tools and session menus with
one program. It's written in C++ and GTK 4, everything runs natively on
Wayland, and the whole look comes from The Beginning After the End.

I build it for my own machine first. Everything else is secondary.

## Screenshots

### Status bar and control panel

![Status bar and control panel](assets/Showcase/Screenshots/TaskbarAndSidebar.png)

### Launcher

![Launcher](assets/Showcase/Screenshots/AppLauncher.png)

### Workspace overview

![Workspace overview](assets/Showcase/Screenshots/WorkspaceOverview.png)

### Wallpaper switcher

![Wallpaper switcher](assets/Showcase/Screenshots/WallpaperSwitcher.png)

### Power menu

![Power menu](assets/Showcase/Screenshots/PowerMenu.png)

### Lock screen

![Lock screen](assets/Showcase/Screenshots/Lockscreen.png)

---

## Showcase Video

https://github.com/user-attachments/assets/0efbbdd1-fc3a-41f3-87fb-a696f99375a5

---

## Features

| | |
| --- | --- |
| **Status bar** | One output-aware bar per monitor with clock, workspace runes and previews, system monitor, media controls, battery, network, notifications, and quick notes. |
| **Launcher** | Monitor-local app search with usage-aware ranking, window focusing, calculator, fish command execution, clipboard history, emoji search. |
| **Control panel** | Monitor-local Wi-Fi, Bluetooth, volume, brightness, Night Light, power profiles, GameMode, notification history. |
| **Lock screen** | Custom Broken Seal lock surfaces across all active outputs with real PAM authentication; `hyprlock` is only a fail-closed fallback. |
| **Power menu** | Output-local fullscreen animated scene with lock, suspend, logout, reboot, and power-off; standard, ultrawide, and super-ultrawide layouts keep the video and controls correctly composed. |
| **Wallpaper engine** | Native Wayland/EGL renderer with per-output wallpapers, connector-based persistence, hotplug handling, output-aware cropping, and smooth transitions. Wallpaper changes also regenerate the theme colors. |
| **Wallpaper switcher** | Carousel overlay to cycle through and preview wallpapers; on multi-monitor setups it applies to the monitor where the selector was opened. |
| **Notifications** | D-Bus server, transient toasts, unread state, persistent history. |
| **Utilities** | Screenshots, region capture, screen recording, brightness/volume OSDs. |
| **Hyprland FX plugin** | Required compositor-side rendering for window transitions and the lock-screen/power-menu presentation. |



---

## Dependencies

### Build

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkgconf gtest \
  gtk4 gtk4-layer-shell glib2 gdk-pixbuf2 \
  libjpeg-turbo libepoxy
```

The native wallpaper renderer and compositor integration use:

```bash
sudo pacman -S --needed wayland wayland-protocols wlr-protocols mesa
```

`realmheart-fx.so` is an essential part of the shell. It builds against the
currently installed Hyprland plugin ABI, so the matching Hyprland development
files must be available through `pkg-config`:

```bash
pkg-config --exists hyprland glesv2
```

On the supported Arch/CachyOS baseline, the normal Hyprland and Mesa packages
provide these files. Treat CMake's `Realmheart FX plugin disabled` message as a
missing dependency, not a harmless optional-build notice.

### Runtime

Core: `hyprctl`, `brightnessctl`, `wpctl`, `nmcli`, `bluetoothctl`,
`powerprofilesctl`, `matugen`.

Optional extras:

| Command | Enables |
| --- | --- |
| `grim` + `slurp` | Screenshots and region capture |
| `wl-clipboard` + `cliphist` | Clipboard integration and history |
| `wf-recorder` | Screen recording |
| `hyprlock` | Emergency fail-closed fallback if the custom Broken Seal lock cannot safely cover every active output |
| `hypridle` | Idle/session integration |
| `hyprsunset` | Night Light |

---

## Install and run

```bash
cd ~
git clone https://github.com/kzahed610/Realmheart.git
cd Realmheart

# ⚠️  FRESH ACCOUNT / COPIED FROM ANOTHER MACHINE?
# The build tree (build-hybrid/) does NOT transfer between accounts or machines.
# It bakes your home path into CMAKE_SOURCE_DIR; a stale build-hybrid/ copied in
# from another checkout produces the cryptic
#   "CMakeCache.txt directory ... is different" / "Permission denied" errors.
# Always nuke any pre-existing build dir first (no-op if it isn't there):
rm -rf build-hybrid

# IMPORTANT: never run cmake/build with sudo. Running as root changes the
# effective user and trips CMake's source-cache mismatch check. sudo is only
# needed for the system PAM service (see install-hypr-configs.sh).
cmake -S . -B build-hybrid -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DREALMHEART_ENABLE_NATIVE_WALLPAPER=ON \
  -DBUILD_TESTING=ON

cmake --build build-hybrid -j"$(nproc)"

# Required. If this file is missing, stop and fix the Hyprland/GLES development
# dependencies before installing Realmheart.
test -f build-hybrid/realmheart-fx.so
```

Check your environment before installing the shell integration:

```bash
./build-hybrid/realmheart --doctor
```

### Hyprland config setup

Realmheart ships portable Hyprland configs under `config/`. The installer
copies them into `~/.config/hypr/` and `~/.config/realmheart/`, installs the FX
loader into `~/.local/bin/`, and creates the user unit at
`~/.config/systemd/user/realmheart.service`:

```bash
./install-hypr-configs.sh
```

The installer saves every replaced file as `<file>.bak.<timestamp>` and only
uses sudo when a destination cannot be written normally. The shipped Hyprland
startup hooks start `realmheart.service` and load `realmheart-fx.so`
automatically. Log out and back into Hyprland after the first install; there is
no separate plugin-load or shell-autostart command to maintain.

If you ever see absolute paths from a previous checkout baked into the
binary (the classic "/home/you path error"), delete the build tree and
reconfigure from scratch:

```bash
rm -rf build-hybrid
cmake -S . -B build-hybrid -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DREALMHEART_ENABLE_NATIVE_WALLPAPER=ON
```

### Required FX plugin

The lock screen, power menu and Realmheart's window transitions depend on
`realmheart-fx.so`. `install-hypr-configs.sh` installs both the loader and its
Hyprland startup hook, so loading is automatic. The loader finds the plugin in
`~/Realmheart/build-hybrid/` or the supported local/system install locations
and refuses to load it twice.

The plugin is ABI-coupled to Hyprland. Rebuild Realmheart after every Hyprland
update, then start a fresh Hyprland session so the matching plugin is loaded.

A running shell is controlled without restarting the session:

```bash
realmheart --command <name>
```

Run the test suite with `ctest --test-dir build-hybrid --output-on-failure`.

---

## Supported monitors

Realmheart does not treat the desktop as one fixed 1920×1080 canvas. Every
output gets its own monitor context with independent logical geometry, scale,
layout density, asset density, and connector identity.

### Validated display matrix

| Layout | Resolution / topology | Realmheart behaviour | Status |
| --- | --- | --- | --- |
| 1080p | 1920×1080 | 1080p layout and assets | Stable baseline |
| 1440p | 2560×1440 | 1440p layout and assets | Supported |
| 4K | 3840×2160 | 4K layout and assets | Supported |
| 1080p ultrawide | 2560×1080 | 1080p layout; wider viewport | Supported |
| 1440p ultrawide | 3440×1440 | 1440p layout; wider viewport | Supported |
| 1440p super-ultrawide | 5120×1440 | 1440p layout; super-ultrawide viewport | Supported |
| Mixed resolution | 1920×1080 + 2560×1440 | Independent per-output layout/assets | Supported |
| Mixed resolution | 2560×1440 + 3840×2160 | Independent per-output layout/assets | Supported |
| Mixed DPI | e.g. 3840×2160 at 2× scale | 1080p logical layout with 4K raster assets | Supported |

Layout density is chosen from the output's **logical short edge**, not its raw
width. A 3440×1440 monitor therefore keeps the same UI density as 2560×1440
instead of inflating every control just because the screen is wider. Layout and
asset tiers are resolved separately, so a scaled 4K panel can use comfortable
1080p logical geometry without throwing away 4K artwork.

Multi-monitor behaviour is explicitly output-owned:

- the Status Bar/Taskbar, wallpaper surface, and sidebar hotspot are created per output;
- launcher, sidebar, Notes, Workspace Overview, OSDs, toasts, and the power menu
  open on the monitor where they were invoked and stay bound to that output;
- the wallpaper selector applies only to its owning output, with per-connector
  wallpaper state restored after restart;
- Broken Seal covers every active monitor while one PAM-authenticated surface
  owns keyboard input; a successful unlock closes all lock surfaces together;
- ultrawide Workspace Overview and power-menu media preserve aspect ratio instead
  of stretching 16:9 artwork, while interactive controls stay inside the real
  monitor viewport;
- output hotplug/reconfiguration rebuilds monitor-bound shell surfaces instead
  of assuming a permanent monitor 0.

### Display diagnostics and isolated testing

Inspect the compiled display contracts and asset provenance with:

```bash
./build-hybrid/realmheart --resolution-status
```

Realmheart also ships an isolated nested-Hyprland harness for testing layouts
without changing the physical monitor configuration. Interactive mode needs
`wayvnc`, `gtk-vnc`, and `python-gobject`:

```bash
sudo pacman -S --needed wayvnc gtk-vnc python-gobject

python tools/display-tests/isolated_hyprland.py ultrawide-1080 --interactive
python tools/display-tests/isolated_hyprland.py ultrawide-1440 --interactive
python tools/display-tests/isolated_hyprland.py super-ultrawide-1440 --interactive
python tools/display-tests/isolated_hyprland.py dual-1080-1440 --interactive
python tools/display-tests/isolated_hyprland.py dual-1440-4k --interactive
python tools/display-tests/isolated_hyprland.py mixed-dpi --interactive
```

Multi-output layouts open one viewer per virtual monitor. Clicking inside a
viewer makes that output the invocation target, which allows monitor ownership,
per-output wallpapers, mixed-density surfaces, and lock-screen coverage to be
validated interactively.

---

## Versions and compatibility

Realmheart moves with Hyprland. The shell needs the Lua config era, meaning
Hyprland **0.55 or newer**. Development happens against **0.56.x**, which is
where it gets battle-tested daily. If you pin an older Realmheart tag, match
it roughly to a Hyprland release from the same period.

Currently used elsewhere: GTK 4.12 or newer (development on 4.22),
gtk4-layer-shell 1.3, matugen 4.x.

The required FX plugin compiles against Hyprland's internal plugin ABI and
must be rebuilt after every Hyprland update. The current build targets the
0.56 ABI.

Stable host baseline: CachyOS/Arch Linux, Hyprland 0.56.2, GTK 4.22.
Display compatibility is additionally regression-tested in isolated Hyprland
sessions across the standard, ultrawide, super-ultrawide, mixed-resolution,
and mixed-DPI matrix documented above.

---

## Roadmap

Rough order, no dates:

- A few selected distributions beyond Arch/CachyOS.
- More widgets.
- A settings overlay for customizing Realmheart itself plus some general
  system behaviour, partly aimed at people switching from Windows. Least
  certain item on the list.

None of this is scheduled. It's a hobby project and the roadmap bends toward
whatever turns out to be fun to build.

---

## Notes

- Realmheart is licensed under the GPL-3.0. See [LICENSE](LICENSE).
- The required plugin under `plugins/realmheart-fx/` is ABI-coupled to Hyprland
  and must be rebuilt after Hyprland updates. Extra attribution lives in
  `plugins/realmheart-fx/`.
- Unofficial fan project inspired by The Beginning After the End, not
  affiliated with the creators or rights holders.

---

<p align="center">
  <strong>Realmheart</strong><br />
  Native where it matters. Excessive where it counts.
</p>
