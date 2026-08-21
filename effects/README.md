# Effect assets

Realmheart keeps effect assets grouped by the surface that owns their
lifecycle and rendering contract:

```text
effects/
├── windows/       Hyprland window-transition effect packages
├── power-menu/    Power-menu-only shaders and supporting effect assets
├── workspace/     Workspace-overview transition shaders
└── lockscreen/    Broken Seal lockscreen shaders (horn pair)
```

Window effects are manifest-driven. Each direct child of `effects/windows/`
contains an `effect.toml` and its shader files. The Realmheart FX plugin scans
only that directory.

Power-menu effects are owned by the native shell and must stay under
`effects/power-menu/`; they are not window-effect manifests and are never
loaded by the Hyprland plugin registry.

Workspace effects are likewise native-shell assets. They are owned by the
workspace overview's transition renderer and must stay under
`effects/workspace/`; they are not window-effect manifests.

Lockscreen effects are native-shell assets owned by the Broken Seal surface's
`ShaderManager` (hot-reloaded at 250 ms) and must stay under
`effects/lockscreen/`; they are not window-effect manifests.
