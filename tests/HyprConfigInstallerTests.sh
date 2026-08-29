#!/usr/bin/env bash
set -euo pipefail

REPO="${1:?repository root is required}"
TEMP_HOME="$(mktemp -d -t realmheart-installer-test-XXXXXX)"
cleanup() {
    rm -rf "$TEMP_HOME"
}
trap cleanup EXIT

FIRST_OUTPUT="$TEMP_HOME/first-run.log"
SECOND_OUTPUT="$TEMP_HOME/second-run.log"

env -u SUDO_USER HOME="$TEMP_HOME" \
    "$REPO/install-hypr-configs.sh" >"$FIRST_OUTPUT" 2>&1

FX_DEST="$TEMP_HOME/.config/hypr/realmheart_fx.lua"
ENTRY_DEST="$TEMP_HOME/.config/hypr/hyprland.lua"
SERVICE_DEST="$TEMP_HOME/.config/systemd/user/realmheart.service"

[[ -f "$FX_DEST" ]] || {
    echo "realmheart_fx.lua was not installed" >&2
    exit 1
}
[[ -f "$ENTRY_DEST" ]] || {
    echo "hyprland.lua was not installed" >&2
    exit 1
}
[[ -f "$SERVICE_DEST" ]] || {
    echo "realmheart.service was not installed" >&2
    exit 1
}

FX_LINE=$(grep -nF "installed: $FX_DEST" "$FIRST_OUTPUT" | cut -d: -f1)
ENTRY_LINE=$(grep -nF "installed: $ENTRY_DEST" "$FIRST_OUTPUT" | cut -d: -f1)
[[ -n "$FX_LINE" && -n "$ENTRY_LINE" && "$FX_LINE" -lt "$ENTRY_LINE" ]] || {
    echo "hyprland.lua was installed before realmheart_fx.lua; first-run auto-reload can observe an incomplete config tree" >&2
    exit 1
}

EXPECTED_EXEC="ExecStart=$REPO/build-hybrid/realmheart --shell --wallpaper-backend native"
grep -Fqx "$EXPECTED_EXEC" "$SERVICE_DEST" || {
    echo "realmheart.service does not point at the repository build" >&2
    exit 1
}
grep -Fqx 'Restart=on-failure' "$SERVICE_DEST"
grep -Fqx 'PartOf=graphical-session.target' "$SERVICE_DEST"

grep -Fq 'systemctl --user start realmheart.service' \
    "$TEMP_HOME/.config/hypr/hyprland/execs.lua" || {
    echo "Hyprland startup does not start realmheart.service" >&2
    exit 1
}

env -u SUDO_USER HOME="$TEMP_HOME" \
    "$REPO/install-hypr-configs.sh" >"$SECOND_OUTPUT" 2>&1

compgen -G "$SERVICE_DEST.bak.*" >/dev/null || {
    echo "second run did not back up the existing service unit" >&2
    exit 1
}

printf 'Realmheart installer clean-home and rerun contracts passed.\n'
