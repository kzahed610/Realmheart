# Realmheart

Native C++ Hyprland shell rewrite for Zahed's desktop. TBATE reference mandatory; sanity optional.

## 🎯 Project Goals
- **Efficiency:** Lower idle RAM than the current Quickshell/QML shell.
- **Focus:** Implement only the features that are actually used.
- **Architecture:** Maintain a clean, modular codebase that avoids "soup" and allows easy addition of future features/animations.

## 🚀 Current Capabilities
Realmheart is now a fully functional desktop shell providing the following:

### 🖼️ Display & Visuals
- **Hybrid Wallpaper Engine:** Supports both a GTK4-based fallback and a high-performance native Wayland/EGL/OpenGL ES renderer.
- **Dynamic Theming:** Integrated `Matugen` parser for automatic color palette generation based on wallpaper, ensuring systemic visual consistency.
- **OSD Overlays:** Lean, non-intrusive On-Screen Displays for system adjustments.

### 📊 Interface Components
- **Aether Spine taskbar:** A slim Matugen-themed left rail with a hard-gold
  outward contour, rune workspaces, hover window previews, on-demand system and
  battery details, dynamic Wi-Fi/notification state, and a compact MPRIS media
  popup.
- **Right Sidebar:** A centralized control hub for:
  - **Connectivity:** WiFi and Bluetooth status/controls.
  - **System Toggles:** Gamemode, Night Light, and "Keep Awake" (inhibitor).
  - **Hardware Sliders:** Real-time Brightness and Volume control.
  - **Power Management:** Power profile cycling.
  - **Notification Center:** Integrated D-Bus capture with a scrollable history list.
- **Notes Overlay:** Simple, plaintext-backed persistent notes for quick access.
- **Launcher:** Dedicated overlay for application launching via `realmheart-cli`.

### 🔒 System Integration
- **Lock Screen:** A faithful C++ implementation of the  design and behavior.
- **Core Control:** A robust command-and-capture system allowing the shell to be controlled and updated via CLI without full session restarts.
- **Diagnostic Suite:** `--doctor` mode for dependency probing and environment validation.

## 🗺️ Implementation Roadmap

### Phase 1-5: Core & UI (Complete)
- [x] Core command execution, Layer-shell abstractions, and diagnostics.
- [x] Service layer for Power, Brightness, Audio, and Workspaces.
- [x] Modular Vertical Bar with asynchronous probing.
- [x] Full Right Sidebar with D-Bus notification integration.
- [x] Lock screen mirroring the  design.
- [x] RAM optimization and performance audit.

### Phase 6: Future Expansions (Planned)
- [ ] **Left Sidebar** - Implementation of an empty container for future modular expansion.
- [x] **Active State Mutation** - Right Sidebar controls mutate and confirm Brightness, Volume, Bluetooth, Gamemode, Night Light, Keep Awake, and power profiles asynchronously.
- [x] **Dynamic Background** - Wallpaper management integrated with Matugen theming and native/GTK fallback backends.
- [x] **Screenshot Pipeline** - Full-screen, region-to-clipboard, OCR-region, and recording helpers.

## 🛠️ Build & Run

### Installation
GTK 4.12+, `gtk4-layer-shell-0`, and `gdk-pixbuf-2.0` are required through pkg-config.
The optional native wallpaper renderer additionally uses Wayland, EGL, OpenGL ES 2, `wayland-scanner`, and the wlr-protocol XML files.
`curl` is optional and is used only to cache remote MPRIS album art.

### Commands
```sh
# Configure and Build
cmake -S . -B build -G Ninja
cmake --build build

# Run the registered test suite
ctest --test-dir build --output-on-failure

# Optional system-wide install
cmake --install build

# Diagnostics
./build/realmheart --doctor

# Start with either wallpaper backend
./build/realmheart --shell --wallpaper-backend gtk
./build/realmheart --shell --wallpaper-backend native

# Switch a running shell without restarting
./build/realmheart --command set-wallpaper-backend gtk
./build/realmheart --command set-wallpaper-backend native

# Testing Surfaces
./build/realmheart --test-layer        # Test a basic layer-shell surface
./build/realmheart --bar --timeout 5   # Test the Vertical Bar
./build/realmheart --sidebar --timeout 1 # Test the Right Sidebar
./build/realmheart --workspace-status  # Test workspace service
```

## 🖼️ Hybrid Wallpaper Architecture
Wallpaper state is independent from rendering. Realmheart can use the existing GTK4 backend or a separate Wayland/EGL/OpenGL ES renderer, and can switch between them while running. The GTK backend remains an automatic fallback if the native renderer is missing or fails. See [`docs/wallpaper-backends.md`](docs/wallpaper-backends.md).

## 📋 Confirmed Scope
- **Right Sidebar:** WiFi, Bluetooth, Keep Awake, Night Light, Gamemode, power-profile cycle, brightness slider, volume slider, and notification history/list.
- **Notes:** Plaintext file persistence; no encryption/keyring gates.
- **Lock Screen:** Exact visual and behavioral match to the current design.
- **RAM Audit:** Confirmed optimal resource usage across all shell components.

## Reloading the development shell
Realmheart exposes a real restart command that preserves the active wallpaper backend:

```bash
./build-hybrid/realmheart --command restart
```

For Hyprland development, bind `SUPER+R` to your own absolute build path. For example, after setting `REALMHEART_ROOT` in your environment:

```ini
bind = SUPER, R, exec, $REALMHEART_ROOT/build/realmheart --command restart
```

Installed builds can use the executable from `PATH` directly:

```ini
bind = SUPER, R, exec, realmheart --command restart
```
