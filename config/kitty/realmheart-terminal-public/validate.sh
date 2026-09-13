#!/usr/bin/env bash
set -euo pipefail

XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-"$HOME/.config"}
XDG_STATE_HOME=${XDG_STATE_HOME:-"$HOME/.local/state"}
GEN="$XDG_CONFIG_HOME/realmheart/scripts/terminal/generate-theme.py"
FISH_THEME="$XDG_CONFIG_HOME/fish/conf.d/realmheart-theme.fish"
FISH_STARSHIP="$XDG_CONFIG_HOME/fish/conf.d/realmheart-starship.fish"
STARSHIP_CONFIG_PATH="$XDG_STATE_HOME/realmheart/theme/starship.toml"
KITTY_THEME="$XDG_STATE_HOME/realmheart/theme/kitty-theme.conf"

python3 -m py_compile "$GEN"
python3 - "$STARSHIP_CONFIG_PATH" <<'PY'
import sys
import tomllib
from pathlib import Path
text = Path(sys.argv[1]).read_text()
tomllib.loads(text)
assert "@@" not in text
PY

if command -v fish >/dev/null 2>&1; then
    fish -n "$FISH_THEME"
    fish -n "$FISH_STARSHIP"
fi

if command -v starship >/dev/null 2>&1; then
    STARSHIP_CONFIG="$STARSHIP_CONFIG_PATH" starship prompt >/dev/null
fi

test -s "$KITTY_THEME"
printf '%s\n' 'Realmheart terminal validation passed.'
