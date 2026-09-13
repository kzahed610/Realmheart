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
  base-devel cmake ninja pkgconf gtest sqlite \
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

Realmheart ships a transactional Python installer. **Run it as your normal desktop
user, never with `sudo`**; it requests narrow elevation only for package/system
operations that actually need it.

Current first-class install target: **CachyOS/Arch + Wayland + Hyprland**. Realmheart
requires Hyprland **0.56.1 or newer**; the installer treats the tested 0.56.x/0.57.x
lines separately from unknown future ABI revisions because `realmheart-fx.so` is
Hyprland-ABI-sensitive.

```bash
cd ~
git clone https://github.com/kzahed610/Realmheart.git
cd Realmheart

# Inspect the complete environment/dependency/config/system mutation plan first.
python3 installer/realmheart_installer.py --dry-run install

# If the plan is READY, install. On CachyOS/Arch this also allows the verified
# pacman adapter to install missing package-provided dependencies after consent.
python3 installer/realmheart_installer.py --install-dependencies install
```

The installer builds Realmheart as the normal user into an isolated CMake
`DESTDIR`, validates that staged payload, creates the appropriate baseline or
rollback snapshot, and only then enters the journaled live transaction. It owns
the complete Hyprland config tree while preserving `hypr/custom/`; Kitty and Fish
use surgical managed/drop-in integration rather than replacing personal config.
Privileged auth-helper/PAM changes are separately verified and rollback-accounted.

Before the final keep/rollback decision, the installer also runs the bundled
**Realmheart Doctor Acceptance MVP** as an independent, read-only second opinion.
Doctor re-observes canonical artifacts and dependency capabilities from the live
machine and returns `KEEP`, `KEEP_WITH_WARNINGS`, `REVERT_RECOMMENDED`, or
`INDETERMINATE`. Doctor never performs rollback itself and a Doctor crash cannot
turn an otherwise verified install into a destructive failure; the installer
remains the owner of the transaction and final receipt.

After the first successful install, start a **fresh Hyprland session**. A deployment
can be install-verified while runtime activation remains
`pending_session_restart`; the installer deliberately does not claim the new shell
or FX plugin is active until a fresh session can prove it.

Useful safety/recovery commands:

```bash
# Read-only current plan / environment
python3 installer/realmheart_installer.py --dry-run install

# Check whether an interrupted transaction needs attention
python3 installer/realmheart_installer.py recovery-list

# Preview uninstall without changing anything
python3 installer/realmheart_installer.py --dry-run uninstall
python3 installer/realmheart_installer.py --compare-config uninstall

# Bundled read-only Doctor entry point
realmheart-doctor --version
```

See [`installer/README.md`](installer/README.md) for dependency handling,
verification, recovery, diagnostics, uninstall semantics, and the validation
contract.

> **Do not use `install-hypr-configs.sh` for new installs.** It is retained only
> as a guarded legacy compatibility/test fixture so the new installer can keep
> proving adoption behavior for old Realmheart installations.

### Developer build

If you are developing Realmheart rather than installing it, use a clean local
build tree. CMake intentionally refuses to reuse a cache created from a different
source checkout because source-root paths are compiled into parts of the runtime.

```bash
rm -rf build-hybrid
cmake -S . -B build-hybrid -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DREALMHEART_ENABLE_NATIVE_WALLPAPER=ON \
  -DBUILD_TESTING=ON
cmake --build build-hybrid -j"$(nproc)"
ctest --test-dir build-hybrid --output-on-failure
```

`realmheart-fx.so` is required by the supported shell path. A developer build that
cannot produce it should be treated as an incomplete Realmheart build rather than
silently installing without FX.

A running installed shell can be controlled without restarting the session:

```bash
realmheart --command <name>
```

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

Realmheart moves with Hyprland. The current release requires Hyprland
**0.56.1 or newer**. The installer treats **0.56.x and 0.57.x** as tested lines,
with 0.56.2 retained as the preferred development baseline; newer minor lines
are compatibility-unknown until Realmheart FX ABI support is validated.

Currently used elsewhere: GTK 4.12 or newer (development on 4.22),
gtk4-layer-shell 1.3, matugen 4.x.

The required FX plugin compiles against Hyprland's internal plugin ABI and
must be rebuilt after every Hyprland update. The installer binds the build to
the exact detected Hyprland build/ABI identity rather than assuming minor-line
compatibility is sufficient for a previously built plugin.

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
