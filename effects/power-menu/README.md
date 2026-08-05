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

## Renderer process lifecycle

The persistent shell does not instantiate `PowerMenuScene`. It launches the
sibling `realmheart-power-menu-renderer` executable on demand and sends a
`close` command over a private Unix socket when the menu is dismissed. The
helper owns the GTK media pipeline, GStreamer GL workers and ripple `GtkGLArea`,
then exits after the closing animation. This process boundary is intentional:
it lets the kernel reclaim all decoder and graphics-driver allocations instead
of retaining them in the long-lived shell.
