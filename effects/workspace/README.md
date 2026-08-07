# Workspace overview effects

Workspace-overview transition shaders live here. They are loaded directly by
the native workspace overview and are never scanned by the Hyprland
window-effect registry.

## Elemental morph

`elemental-morph/elemental-morph.frag` is a transition-only enhancement layer
for the taskbar-rune-to-overview morph. The geometry renderer remains visible
underneath and owns the structural proxy, rail and realm-band motion. The
shader receives those same frozen band bounds and advancing frontier positions,
then adds the Fire, Water, Wind and Earth breakup around each frontier.

The renderer captures one exact terminal overview scene per transition. Rapid
direction reversals reuse that capture and the same timeline. Near either exact
endpoint the GL overlay fades to zero, leaving the native geometry frame as the
pixel-exact handoff. Any capture, GLES, compile or draw failure hides the GL
layer and automatically preserves the geometry-only animation.
