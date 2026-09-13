#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-"$HOME/.config"}
XDG_STATE_HOME=${XDG_STATE_HOME:-"$HOME/.local/state"}

install -Dm644 "$ROOT/files/.config/kitty/realmheart-theme.conf" \
    "$XDG_CONFIG_HOME/kitty/realmheart-theme.conf"
# Resolve the state root at install time so a non-default XDG layout remains
# portable without embedding any user's home path in the shipped files.
printf '%s\n' \
    '# Realmheart terminal theme include.' \
    '# The generator refreshes this state file from the active Matugen palette.' \
    "include $XDG_STATE_HOME/realmheart/theme/kitty-theme.conf" \
    > "$XDG_CONFIG_HOME/kitty/realmheart-theme.conf"
install -Dm644 "$ROOT/files/.config/fish/conf.d/realmheart-theme.fish" \
    "$XDG_CONFIG_HOME/fish/conf.d/realmheart-theme.fish"
install -Dm644 "$ROOT/files/.config/fish/conf.d/realmheart-starship.fish" \
    "$XDG_CONFIG_HOME/fish/conf.d/realmheart-starship.fish"
install -Dm755 "$ROOT/files/.config/realmheart/scripts/terminal/generate-theme.py" \
    "$XDG_CONFIG_HOME/realmheart/scripts/terminal/generate-theme.py"
install -Dm644 "$ROOT/files/.config/systemd/user/realmheart-terminal-theme.path" \
    "$XDG_CONFIG_HOME/systemd/user/realmheart-terminal-theme.path"
install -Dm644 "$ROOT/files/.config/systemd/user/realmheart-terminal-theme.service" \
    "$XDG_CONFIG_HOME/systemd/user/realmheart-terminal-theme.service"

KITTY_CONFIG="$XDG_CONFIG_HOME/kitty/kitty.conf"
KITTY_INCLUDE="$XDG_CONFIG_HOME/kitty/realmheart-theme.conf"
mkdir -p "$(dirname -- "$KITTY_CONFIG")"
touch "$KITTY_CONFIG"
include_line="include $KITTY_INCLUDE"
if ! grep -Fqx "$include_line" "$KITTY_CONFIG"; then
    printf '\n# BEGIN Realmheart Terminal Theme (managed)\n%s\n# END Realmheart Terminal Theme (managed)\n' \
        "$include_line" >> "$KITTY_CONFIG"
fi

GEN="$XDG_CONFIG_HOME/realmheart/scripts/terminal/generate-theme.py"
"$GEN"

if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload
    systemctl --user enable --now realmheart-terminal-theme.path
    systemctl --user restart realmheart-terminal-theme.service
else
    printf '%s\n' 'warning: systemctl not found; run the generator manually and configure the watcher.' >&2
fi

printf '%s\n' 'Realmheart terminal theme installed.'
