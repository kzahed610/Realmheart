# Power-menu effects

Power-menu-specific shaders and supporting effect assets live here.

Create one subdirectory per animation or visual effect, for example:

```text
effects/power-menu/
└── mana-pulse/
    ├── mana-pulse.frag
    └── supporting assets
```

These assets are loaded by the native power-menu implementation. Do not add an
`effect.toml` intended for the Hyprland window-effect registry here.

## Ripple reveal

`ripple-reveal/ripple-reveal.frag` is the full-screen transition used by the
native power menu. Its origin is supplied from the real taskbar power button,
so the transparent reveal remains aligned when monitor dimensions or bar
geometry change. The shader is transition-only; the normal GTK video resumes
at the terminal frame and the captured texture is released.
