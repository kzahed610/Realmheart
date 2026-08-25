#include "services/ScopeModules.hpp"

namespace realmheart::services {

realmheart::core::ModuleRegistry build_confirmed_module_registry() {
    using realmheart::core::ModuleDescriptor;
    using realmheart::core::ModulePhase;

    realmheart::core::ModuleRegistry registry;

    registry.add(ModuleDescriptor{
        "primary-bar.vertical-left",
        "Vertical left-edge bar: brightness zone, left sidebar toggle, stats, music, workspaces, clock, battery, tray/status cluster/right-sidebar toggle",
        ModulePhase::Surface,
        {"hyprland", "brightness", "audio", "network", "bluetooth", "battery", "tray", "mpris"},
        true
    });

    registry.add(ModuleDescriptor{
        "sidebar.left.empty-container",
        "Animated empty left sidebar container reserved for future Zahed-designed features",
        ModulePhase::Surface,
        {"animation"},
        true
    });

    registry.add(ModuleDescriptor{
        "sidebar.right.quick-controls-notifications",
        "Right sidebar: WiFi, Bluetooth, Keep Awake, Night Light, Gamemode, power-profile cycle, brightness slider, volume slider, notification history/list",
        ModulePhase::Widget,
        {"network", "bluetooth", "idle-inhibit", "night-light", "gamemode", "power-profiles", "brightness", "audio", "notifications"},
        true
    });

    registry.add(ModuleDescriptor{
        "overlay.container",
        "SUPER+G extensible overlay container with plaintext Notes and Recorder widgets",
        ModulePhase::Overlay,
        {"notes-store", "recorder"},
        true
    });

    registry.add(ModuleDescriptor{
        "lockscreen.broken-seal",
        "Native lockscreen: password entry, PAM auth",
        ModulePhase::Lock,
        {"auth", "background", "blur", "animations", "hyprland"},
        true
    });

    registry.add(ModuleDescriptor{
        "background.wallpaper-theming",
        "Wallpaper rendering plus matugen-driven system palette application",
        ModulePhase::CoreService,
        {"matugen", "wallpaper"},
        true
    });

    registry.add(ModuleDescriptor{
        "overview.workspaces",
        "Workspace/window overview for day-to-day navigation",
        ModulePhase::Surface,
        {"hyprland"},
        true
    });

    registry.add(ModuleDescriptor{
        "screenshot.region-pipeline",
        "Interactive screenshot region selector with resize handles and options toolbar",
        ModulePhase::Surface,
        {"screenshot"},
        true
    });

    return registry;
}

} // namespace realmheart::services
