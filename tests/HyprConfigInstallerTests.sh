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
PAM_DEST="$TEMP_HOME/.config/hypr/realmheart-lockscreen"

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
[[ ! -e "$PAM_DEST" ]] || {
    echo "PAM service was incorrectly installed into the Hyprland config tree" >&2
    exit 1
}

FX_LINE=$(grep -nF "installed: $FX_DEST" "$FIRST_OUTPUT" | cut -d: -f1)
ENTRY_LINE=$(grep -nF "installed: $ENTRY_DEST" "$FIRST_OUTPUT" | cut -d: -f1)
[[ -n "$FX_LINE" && -n "$ENTRY_LINE" && "$FX_LINE" -lt "$ENTRY_LINE" ]] || {
    echo "hyprland.lua was installed before realmheart_fx.lua; first-run auto-reload can observe an incomplete config tree" >&2
    exit 1
}

EXPECTED_EXEC="ExecStart=\"$REPO/build-hybrid/realmheart\" --shell --wallpaper-backend native"
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

env -u SUDO_USER HOME="$TEMP_HOME" \
    "$REPO/install-hypr-configs.sh" >/dev/null 2>&1

backups=( "$SERVICE_DEST".bak.* )
[[ -f "${backups[0]}" && ${#backups[@]} -ge 2 ]] || {
    echo "repeated runs did not create collision-resistant service backups" >&2
    exit 1
}

SYMLINK_HOME="$TEMP_HOME/symlink-home"
SYMLINK_TARGET="$TEMP_HOME/outside-target"
mkdir -p "$SYMLINK_HOME/.config/hypr"
printf 'must remain unchanged\n' >"$SYMLINK_TARGET"
ln -s "$SYMLINK_TARGET" "$SYMLINK_HOME/.config/hypr/realmheart_fx.lua"
if env -u SUDO_USER HOME="$SYMLINK_HOME" "$REPO/install-hypr-configs.sh" >/dev/null 2>&1; then
    echo "installer accepted a symlink destination" >&2
    exit 1
fi
grep -Fqx 'must remain unchanged' "$SYMLINK_TARGET"

SPACED_HOME="$TEMP_HOME/spaced-home"
SPACED_BINARY="$TEMP_HOME/My Realmheart/build-hybrid/realmheart"
mkdir -p "$(dirname "$SPACED_BINARY")"
env -u SUDO_USER HOME="$SPACED_HOME" REALMHEART_BINARY="$SPACED_BINARY" \
    "$REPO/install-hypr-configs.sh" >/dev/null 2>&1
grep -Fqx "ExecStart=\"$SPACED_BINARY\" --shell --wallpaper-backend native" \
    "$SPACED_HOME/.config/systemd/user/realmheart.service"

printf 'Realmheart installer clean-home and rerun contracts passed.\n'
