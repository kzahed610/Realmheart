# Realmheart Terminal — Public Drop-in

Portable Kitty + Fish + Starship integration for the Relic Grimoire v4.2 N1 prompt.

This pack deliberately does **not** include a personal `config.fish`, full `kitty.conf`, generated wallpaper colors, backups, aliases, credentials, or machine-specific paths. It installs drop-in files and preserves the user's existing configuration.

## Install

From this directory:

```bash
./install.sh
```

The installer:

1. installs the Realmheart Fish theme/state loader and Starship integration under `~/.config/fish/conf.d/`;
2. installs the generator under `~/.config/realmheart/scripts/terminal/`;
3. installs the user systemd watcher/service;
4. adds one managed include to the existing Kitty config;
5. generates the current theme, enables the watcher, and reloads Kitty.

The generator reads:

```text
~/.local/state/realmheart/theme-palette.tsv
```

Realmheart/Matugen should write that cache when the wallpaper changes. If it is absent, the generator uses its built-in fallback palette.

## What changes visually

```text
│ ✶ 󰘿 Realmheartsrc󰉋 core ▪▪▫▫▫        󰘬 main · 3s
│ ╰─󰁔
```

N1 provides the Aether Ribbon, Depth Compass, context chips, semantic ANSI colors, dynamic Matugen materials, transient history, and the Kitty ghost rail.

The public service escapes the date format correctly so live Fish theme notifications work without systemd interpreting `%s` and `%N` as unit specifiers.

## Requirements

- Kitty
- Fish 4.3+
- Starship
- Python 3
- systemd user services
- JetBrains Mono Nerd Font recommended
