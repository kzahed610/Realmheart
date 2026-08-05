# Effect assets

Realmheart keeps effect assets grouped by the surface that owns their
lifecycle and rendering contract:

```text
effects/
├── windows/       Hyprland window-transition effect packages
└── power-menu/    Power-menu-only shaders and supporting effect assets
```

Window effects are manifest-driven. Each direct child of `effects/windows/`
contains an `effect.toml` and its shader files. The Realmheart FX plugin scans
only that directory.

Power-menu effects are owned by the native shell and must stay under
`effects/power-menu/`; they are not window-effect manifests and are never
loaded by the Hyprland plugin registry.
