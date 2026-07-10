# Realmheart

Native C++ Hyprland shell rewrite for Zahed's desktop. TBATE reference mandatory; sanity optional.

## 🎯 Project Goals
- **Efficiency:** Lower idle RAM than the current Quickshell/QML shell.
- **Focus:** Implement only the features that are actually used.
- **Architecture:** Maintain a clean, modular codebase that avoids "soup" and allows easy addition of future features/animations.

## 🗺️ Implementation Roadmap

### Phase 1: Core Infrastructure (Complete)
- [x] Core command execution and capture system.
- [x] Dependency diagnostics and environment probing (`--doctor`).
- [x] Layer-shell abstractions for Wayland surfaces.
- [x] Basic logging and error handling.

### Phase 2: Service Layer (Complete)
- [x] Service-layer primitives for system probing.
- [x] Power profile cycling model.
- [x] Brightness and audio probe helpers.
- [x] Hyprland workspace snapshot service.

### Phase 3: Vertical Bar (Complete)
- [x] Modular `VerticalBar` presentation module.
- [x] Asynchronous, bounded status probes for system states.
- [x] Bounded in-memory notification history model.
- [x] Live bar pills for workspace status.
- [x] Deterministic test suite for bar presentation and state.

### Phase 4: Right Sidebar (Complete)
- [x] **T4.1: Foundation Layout** - Basic window and layout skeleton.
- [x] **T4.2: System Service Integration** - Working controls for Brightness, Power Profile, and Status labels.
- [x] **T4.3: Interactive Control Logic** - Full wiring of toggles, sliders, and cycle buttons.
- [x] **T4.4: Notification History List** - UI implementation of the history list.
- [x] **T4.5: D-Bus Notification Capture** - Attaching to the notification daemon.

### Phase 5: Lock Screen & Final Polish (Complete)
- [x] Lock screen implementation (mirroring  design).
- [x] Final RAM optimization and performance audit.
- [ ] Transition to `~/.config` and live deployment.

## 🛠️ Build & Run

### Installation
GTK4 and `gtk4-layer-shell-0` are required through pkg-config.

### Commands
```sh
# Configure and Build
cmake -S . -B build -G Ninja
cmake --build build

# Diagnostics
./build/realmheart --doctor

# Testing Surfaces
./build/realmheart --test-layer        # Test a basic layer-shell surface
./build/realmheart --bar --timeout 5   # Test the Vertical Bar
./build/realmheart --sidebar --timeout 1 # Test the Right Sidebar
./build/realmheart --workspace-status  # Test workspace service
```

## 📋 Confirmed Scope
- **Right Sidebar:** WiFi, Bluetooth, Keep Awake, Night Light, Gamemode, power-profile cycle, brightness slider, volume slider, and notification history/list.
- **Notes:** Plaintext file persistence; no encryption/keyring gates.
- **Lock Screen:** Exact visual and behavioral match to the current design.
- **RAM Audit:** Confirmed optimal resource usage across all shell components (bounded buffers, stateless services, lean UI).
- **Development Root:** `/home/zahed/Realmheart`
