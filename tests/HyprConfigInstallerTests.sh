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
SYSTEM_ROOT="$TEMP_HOME/system-root"
HELPER_DEST="$SYSTEM_ROOT/usr/local/libexec/realmheart/realmheart-auth-helper"
INSTALL_ENV=(
    env -u SUDO_USER HOME="$TEMP_HOME"
    REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL=1
    REALMHEART_SYSTEM_ROOT="$SYSTEM_ROOT"
    REALMHEART_AUTH_HELPER_DESTINATION="$HELPER_DEST"
    REALMHEART_AUTH_HELPER_SOURCE="$REPO/build-hybrid/realmheart-auth-helper"
)

"${INSTALL_ENV[@]}" "$REPO/install-hypr-configs.sh" >"$FIRST_OUTPUT" 2>&1

FX_DEST="$TEMP_HOME/.config/hypr/realmheart_fx.lua"
ENTRY_DEST="$TEMP_HOME/.config/hypr/hyprland.lua"
SERVICE_DEST="$TEMP_HOME/.config/systemd/user/realmheart.service"
PAM_DEST="$SYSTEM_ROOT/etc/pam.d/realmheart-lockscreen"
LOCK_DEST="$TEMP_HOME/.local/bin/realmheart-lock-session"

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
[[ -x "$LOCK_DEST" ]] || {
    echo "native lock command wrapper was not installed" >&2
    exit 1
}
[[ -f "$PAM_DEST" ]] || {
    echo "PAM service was not installed into the staged system root" >&2
    exit 1
}
[[ -f "$HELPER_DEST" ]] || {
    echo "secure auth helper was not installed into the staged system root" >&2
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
grep -Fqx "binary=$REPO/build-hybrid/realmheart" "$LOCK_DEST"
grep -Fq 'if [[ ! -x "$binary" ]]; then' "$LOCK_DEST"
grep -Fq 'if ! "$binary" --command lock-session "$$-$(/usr/bin/date +%s%N)"; then' "$LOCK_DEST"
if grep -Fq 'status_file=' "$LOCK_DEST"; then
    echo "lock wrapper trusts a forgeable status file" >&2
    exit 1
fi
if grep -Fq '/usr/bin/hyprlock' "$LOCK_DEST"; then
    echo "lock wrapper bypasses ShellRuntime with a direct hyprlock fallback" >&2
    exit 1
fi
if grep -Fq 'fallback_lock()' "$LOCK_DEST"; then
    echo "lock wrapper owns a duplicate fallback path" >&2
    exit 1
fi
grep -Fqx '    /usr/bin/systemctl suspend || /usr/bin/loginctl suspend' "$LOCK_DEST"

grep -Fq 'systemctl --user start realmheart.service' \
    "$TEMP_HOME/.config/hypr/hyprland/execs.lua" || {
    echo "Hyprland startup does not start realmheart.service" >&2
    exit 1
}

"${INSTALL_ENV[@]}" "$REPO/install-hypr-configs.sh" >"$SECOND_OUTPUT" 2>&1

"${INSTALL_ENV[@]}" "$REPO/install-hypr-configs.sh" >/dev/null 2>&1

backups=( "$SERVICE_DEST".bak.* )
[[ -f "${backups[0]}" && ${#backups[@]} -ge 2 ]] || {
    echo "repeated runs did not create collision-resistant service backups" >&2
    exit 1
}
lock_backups=( "$LOCK_DEST".bak.* )
[[ -f "${lock_backups[0]}" && ${#lock_backups[@]} -ge 2 ]] || {
    echo "repeated runs did not create collision-resistant lock-command backups" >&2
    exit 1
}

SYMLINK_HOME="$TEMP_HOME/symlink-home"
SYMLINK_TARGET="$TEMP_HOME/outside-target"
mkdir -p "$SYMLINK_HOME/.config/hypr"
printf 'must remain unchanged\n' >"$SYMLINK_TARGET"
ln -s "$SYMLINK_TARGET" "$SYMLINK_HOME/.config/hypr/realmheart_fx.lua"
if env -u SUDO_USER HOME="$SYMLINK_HOME" \
    REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL=1 \
    REALMHEART_SYSTEM_ROOT="$SYSTEM_ROOT" \
    REALMHEART_AUTH_HELPER_DESTINATION="$HELPER_DEST" \
    REALMHEART_AUTH_HELPER_SOURCE="$REPO/build-hybrid/realmheart-auth-helper" \
    "$REPO/install-hypr-configs.sh" >/dev/null 2>&1; then
    echo "installer accepted a symlink destination" >&2
    exit 1
fi
grep -Fqx 'must remain unchanged' "$SYMLINK_TARGET"

SPACED_HOME="$TEMP_HOME/spaced-home"
SPACED_BINARY="$TEMP_HOME/My Realmheart/build-hybrid/realmheart"
mkdir -p "$(dirname "$SPACED_BINARY")"
env -u SUDO_USER HOME="$SPACED_HOME" REALMHEART_BINARY="$SPACED_BINARY" \
    REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL=1 \
    REALMHEART_SYSTEM_ROOT="$SYSTEM_ROOT" \
    REALMHEART_AUTH_HELPER_DESTINATION="$HELPER_DEST" \
    REALMHEART_AUTH_HELPER_SOURCE="$REPO/build-hybrid/realmheart-auth-helper" \
    "$REPO/install-hypr-configs.sh" >/dev/null 2>&1
grep -Fqx "ExecStart=\"$SPACED_BINARY\" --shell --wallpaper-backend native" \
    "$SPACED_HOME/.config/systemd/user/realmheart.service"

CUSTOM_HOME="$TEMP_HOME/custom-prefix-home"
CUSTOM_SYSTEM_ROOT="$TEMP_HOME/custom-prefix-system"
CUSTOM_BINARY="/opt/realmheart/bin/realmheart"
CUSTOM_HELPER="$CUSTOM_SYSTEM_ROOT/opt/realmheart/libexec/realmheart/realmheart-auth-helper"
env -u SUDO_USER HOME="$CUSTOM_HOME" REALMHEART_BINARY="$CUSTOM_BINARY" \
    REALMHEART_INSTALL_PREFIX=/opt/realmheart \
    REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL=1 \
    REALMHEART_SYSTEM_ROOT="$CUSTOM_SYSTEM_ROOT" \
    REALMHEART_AUTH_HELPER_SOURCE="$REPO/build-hybrid/realmheart-auth-helper" \
    "$REPO/install-hypr-configs.sh" >/dev/null 2>&1
[[ -f "$CUSTOM_HELPER" ]] || {
    echo "installer did not derive the auth helper path from the custom prefix" >&2
    exit 1
}
grep -Fqx "ExecStart=\"$CUSTOM_BINARY\" --shell --wallpaper-backend native" \
    "$CUSTOM_HOME/.config/systemd/user/realmheart.service"
grep -Fqx "binary=$CUSTOM_BINARY" "$CUSTOM_HOME/.local/bin/realmheart-lock-session"

printf 'Realmheart installer clean-home and rerun contracts passed.\n'
