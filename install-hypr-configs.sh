#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Realmheart Hyprland Config Installer
# =============================================================================
# Copies all portable config files from ./config/ into ~/.config/hypr/,
# ~/.config/realmheart/, and ~/.local/bin/ (for helper executables like the
# FX plugin loader). Creates .bak backups with timestamps before overwriting.
# Safe to re-run — always backs up, never deletes.
#
# Usage:
#   ./install-hypr-configs.sh
#
# If the destination files are root-owned, the script will use sudo.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_SRC="$SCRIPT_DIR/config"
HYPRLAND_DIR="${HOME}/.config/hypr"
REALMHEART_DIR="${HOME}/.config/realmheart"
LOCAL_BIN_DIR="${HOME}/.local/bin"
SYSTEMD_USER_DIR="${HOME}/.config/systemd/user"
REALMHEART_BINARY_PATH="${REALMHEART_BINARY:-$SCRIPT_DIR/build-hybrid/realmheart}"
REALMHEART_INSTALL_PREFIX="${REALMHEART_INSTALL_PREFIX:-/usr/local}"
REALMHEART_SYSTEM_ROOT="${REALMHEART_SYSTEM_ROOT:-}"
if [[ -z "${REALMHEART_AUTH_HELPER_DESTINATION:-}" ]]; then
    if [[ "$REALMHEART_BINARY_PATH" == */bin/realmheart &&
          "$REALMHEART_INSTALL_PREFIX" == "/usr/local" ]]; then
        REALMHEART_INSTALL_PREFIX="$(dirname -- "$(dirname -- "$REALMHEART_BINARY_PATH")")"
    fi
    if [[ -n "$REALMHEART_SYSTEM_ROOT" ]]; then
        REALMHEART_AUTH_HELPER_DESTINATION="${REALMHEART_SYSTEM_ROOT%/}${REALMHEART_INSTALL_PREFIX}/libexec/realmheart/realmheart-auth-helper"
    else
        REALMHEART_AUTH_HELPER_DESTINATION="$REALMHEART_INSTALL_PREFIX/libexec/realmheart/realmheart-auth-helper"
    fi
fi

# If invoked through sudo, resolve the real user's home from SUDO_USER so we
# never write config files into /root. Refuse silently if the variable isn't
# set (e.g. direct root login) so the caller must run as a normal user.
if [[ -n "${SUDO_USER:-}" ]]; then
    REAL_HOME="$(getent passwd "$SUDO_USER" | cut -d: -f6)"
    if [[ -z "$REAL_HOME" ]]; then
        echo "Error: unable to resolve home for SUDO_USER=$SUDO_USER" >&2
        exit 1
    fi
    HYPRLAND_DIR="${REAL_HOME}/.config/hypr"
    REALMHEART_DIR="${REAL_HOME}/.config/realmheart"
    LOCAL_BIN_DIR="${REAL_HOME}/.local/bin"
    SYSTEMD_USER_DIR="${REAL_HOME}/.config/systemd/user"
fi

# Determine whether we need sudo
need_sudo() {
    local dest="$1"
    local dest_dir
    dest_dir="$(dirname "$dest")"
    mkdir -p "$dest_dir" 2>/dev/null || true
    # Atomic replacement and backups need write access to the parent directory.
    if [[ -d "$dest_dir" ]] && [[ -w "$dest_dir" ]] &&
       { [[ ! -e "$dest" ]] || { [[ -r "$dest" ]] && [[ -w "$dest" ]]; }; }; then
        return 1   # false (no sudo)
    fi
    return 0      # true (sudo needed)
}

run_privileged() {
    if [[ -n "${use_sudo:-}" ]]; then
        sudo -- "$@"
    else
        "$@"
    fi
}

path_contains_symlink() {
    local path="$1"
    while [[ "$path" != "/" && -n "$path" ]]; do
        if [[ -L "$path" ]]; then
            return 0
        fi
        path="$(dirname -- "$path")"
    done
    return 1
}

validate_destination() {
    local dest="$1"
    local parent
    parent="$(dirname -- "$dest")"
    if [[ -L "$dest" ]]; then
        echo "  error: refusing symlink destination: $dest" >&2
        return 1
    fi
    if [[ -e "$dest" && ! -f "$dest" ]]; then
        echo "  error: refusing non-regular destination: $dest" >&2
        return 1
    fi
    if path_contains_symlink "$parent"; then
        echo "  error: refusing destination through a symlinked directory: $parent" >&2
        return 1
    fi
    return 0
}

systemd_escape_exec_arg() {
    local value="$1"
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    value=${value//%/%%}
    printf '"%s"' "$value"
}

copy_file() {
    local src="$1"
    local dest="$2"
    local use_sudo=""
    local dest_dir
    local temporary=""
    local backup=""

    if [[ ! -f "$src" || -L "$src" ]]; then
        echo "  error: source is not a regular file: $src" >&2
        return 1
    fi
    validate_destination "$dest" || return 1

    dest_dir="$(dirname -- "$dest")"
    if ! mkdir -p "$dest_dir" 2>/dev/null; then
        :
    fi

    if need_sudo "$dest"; then
        use_sudo="sudo"
    fi

    if ! run_privileged mkdir -p -- "$dest_dir"; then
        echo "  error: unable to create destination directory: $dest_dir" >&2
        return 1
    fi

    if ! temporary="$(run_privileged mktemp -- "$dest_dir/.realmheart-copy.XXXXXX")"; then
        echo "  error: unable to allocate an atomic temporary file for $dest" >&2
        return 1
    fi

    if [[ -f "$dest" ]]; then
        if ! backup="$(run_privileged mktemp -- "${dest}.bak.XXXXXX")" ||
           ! run_privileged cp --preserve=mode,timestamps -- "$dest" "$backup"; then
            run_privileged rm -f -- "$temporary" "$backup"
            echo "  error: unable to create backup for $dest" >&2
            return 1
        fi
        echo "  backed up: $dest -> $backup"
    fi

    if ! run_privileged cp --preserve=mode,timestamps -- "$src" "$temporary" ||
       ! run_privileged mv -f -- "$temporary" "$dest"; then
        run_privileged rm -f -- "$temporary"
        if [[ -n "$backup" ]]; then
            local restore=""
            if restore="$(run_privileged mktemp -- "$dest.restore.XXXXXX")" &&
               run_privileged cp --preserve=mode,timestamps -- "$backup" "$restore" &&
               run_privileged mv -f -- "$restore" "$dest"; then
                echo "  rolled back: $dest" >&2
            else
                run_privileged rm -f -- "$restore"
                echo "  error: rollback failed for $dest; backup retained at $backup" >&2
            fi
        else
            run_privileged rm -f -- "$dest"
        fi
        echo "  error: unable to install $dest" >&2
        return 1
    fi
    echo "  installed: $dest"
}

install_secure_auth_helper() {
    local source="${REALMHEART_AUTH_HELPER_SOURCE:-$(dirname -- "$REALMHEART_BINARY_PATH")/realmheart-auth-helper}"
    local destination="$REALMHEART_AUTH_HELPER_DESTINATION"
    local use_sudo=""
    local metadata=""

    if [[ ! -f "$source" || -L "$source" ]]; then
        echo "  error: auth helper is not a regular file: $source" >&2
        return 1
    fi
    if [[ ! -x "$source" ]]; then
        echo "  error: auth helper is not executable: $source" >&2
        return 1
    fi
    if [[ "${REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL:-0}" == "1" &&
          ( -z "$REALMHEART_SYSTEM_ROOT" ||
            "$destination" != "${REALMHEART_SYSTEM_ROOT%/}/"* ) ]]; then
        echo "  error: unprivileged auth-helper staging must stay inside REALMHEART_SYSTEM_ROOT" >&2
        return 1
    fi

    if need_sudo "$destination"; then
        use_sudo="sudo"
    fi
    local install_command=(install -D -o root -g root -m 4755)
    if [[ "${REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL:-0}" == "1" &&
          -n "$REALMHEART_SYSTEM_ROOT" ]]; then
        # Package staging is intentionally explicit and cannot create a
        # runnable secure helper: the staged file remains user-owned until the
        # privileged package installation sets root:root ownership.
        install_command=(install -D -m 4755)
    fi
    if ! run_privileged "${install_command[@]}" -- "$source" "$destination"; then
        echo "  error: unable to install root-owned setuid auth helper at $destination" >&2
        echo "         run this installer with sudo available, or set REALMHEART_AUTH_HELPER_DESTINATION" >&2
        return 1
    fi
    if ! metadata="$(run_privileged stat -c '%u:%g:%a' -- "$destination")" ||
       { [[ "$metadata" != "0:0:4755" ]] &&
         [[ "${REALMHEART_ALLOW_UNPRIVILEGED_STAGED_INSTALL:-0}" != "1" ]]; }; then
        echo "  error: auth helper failed security metadata check at $destination ($metadata)" >&2
        return 1
    fi
    echo "  installed secure auth helper: $destination"
}

install_lock_command() {
    local binary="$REALMHEART_BINARY_PATH"
    local destination="$LOCAL_BIN_DIR/realmheart-lock-session"
    local temporary
    local quoted_binary
    temporary="$(mktemp -t realmheart-lock-command-XXXXXX)"
    quoted_binary="$(printf '%q' "$binary")"

    if ! {
        printf '%s\n' \
            '#!/usr/bin/env bash' \
            'set -euo pipefail' \
            '' \
            'suspend=0' \
            'case "${1:-}" in' \
            '    "") ;;' \
            '    "--suspend") suspend=1 ;;' \
            '    *) printf "Realmheart: unknown lock command argument: %s\\n" "$1" >&2; exit 2 ;;' \
            'esac' \
            '' \
            "binary=$quoted_binary" \
            'if [[ ! -x "$binary" ]]; then' \
            '    printf "%s\\n" "Realmheart: native lock binary was not found: $binary" >&2' \
            '    exit 127' \
            'fi' \
            'if ! "$binary" --command lock-session "$$-$(/usr/bin/date +%s%N)"; then' \
            '    printf "%s\\n" "Realmheart: native lock request could not be delivered" >&2' \
            '    exit 125' \
            'fi' \
            '' \
            'if (( suspend )); then' \
            '    /usr/bin/systemctl suspend || /usr/bin/loginctl suspend' \
            'fi'
    } >"$temporary"; then
        rm -f -- "$temporary"
        return 1
    fi
    if ! chmod 0755 "$temporary"; then
        rm -f -- "$temporary"
        return 1
    fi

    echo "[realmheart-lock-session]"
    if copy_file "$temporary" "$destination"; then
        rm -f -- "$temporary"
    else
        rm -f -- "$temporary"
        return 1
    fi
}

install_config_source() {
    local src="$1"
    local rel="${src#$CONFIG_SRC/}"
    local dest_base=""

    if [[ "$rel" == hypr/* ]]; then
        dest_base="$HYPRLAND_DIR/${rel#hypr/}"
    elif [[ "$rel" == realmheart/* ]]; then
        dest_base="$REALMHEART_DIR/${rel#realmheart/}"
    elif [[ "$rel" == bin/* ]]; then
        dest_base="$LOCAL_BIN_DIR/${rel#bin/}"
    elif [[ "$rel" == pam/* ]]; then
        dest_base="$REALMHEART_SYSTEM_ROOT/etc/pam.d/${rel#pam/}"
    else
        return 0
    fi

    echo "[$(basename "$dest_base")]"
    copy_file "$src" "$dest_base"
}

install_realmheart_service() {
    # REALMHEART_BINARY is an explicit deployment/test override; normal users
    # get the repository build path used by the historical helper.
    local binary="$REALMHEART_BINARY_PATH"
    local destination="$SYSTEMD_USER_DIR/realmheart.service"
    local temporary
    local escaped_binary
    temporary="$(mktemp -t realmheart-service-XXXXXX)"
    escaped_binary="$(systemd_escape_exec_arg "$binary")"

    if ! {
        printf '%s\n' \
            '[Unit]' \
            'Description=Realmheart desktop shell' \
            'After=graphical-session.target' \
            'PartOf=graphical-session.target' \
            '' \
            '[Service]' \
            'Type=simple' \
            "ExecStart=$escaped_binary --shell --wallpaper-backend native" \
            'Restart=on-failure' \
            'RestartSec=2' \
            '' \
            '[Install]' \
            'WantedBy=graphical-session.target'
    } >"$temporary"; then
        rm -f -- "$temporary"
        return 1
    fi
    if ! chmod 0644 "$temporary"; then
        rm -f -- "$temporary"
        return 1
    fi

    echo "[realmheart.service]"
    if copy_file "$temporary" "$destination"; then
        rm -f -- "$temporary"
    else
        rm -f "$temporary"
        return 1
    fi

    if [[ ! -x "$binary" ]]; then
        echo "  warning: Realmheart binary is not built yet: $binary" >&2
        echo "           Build it before the next Hyprland login." >&2
    fi
}

reload_current_user_manager() {
    local account_home
    account_home="$(getent passwd "$(id -un)" | cut -d: -f6)"

    # A test HOME or sudo-run installer may target another account. Never poke
    # the wrong user manager; the unit is discovered automatically at login.
    if [[ "$HOME" != "$account_home" ]] || ! command -v systemctl >/dev/null 2>&1; then
        echo "  user manager reload deferred until the target user's next login"
        return 0
    fi

    if systemctl --user show-environment >/dev/null 2>&1; then
        systemctl --user daemon-reload
        echo "  reloaded current user systemd manager"
    else
        echo "  user manager unavailable; unit will be discovered at next login"
    fi
}

echo "=== Realmheart Hyprland Config Installer ==="
echo ""

install_secure_auth_helper
install_lock_command

# Install dependencies first. Hyprland watches its entrypoint and may reload as
# soon as hyprland.lua appears; copying it before realmheart_fx.lua exists causes
# a first-run-only `module 'realmheart_fx' not found` warning.
while IFS= read -r -d '' src; do
    rel="${src#$CONFIG_SRC/}"
    case "$rel" in
        hypr/hyprland.lua) continue ;;
    esac
    install_config_source "$src"
done < <(find "$CONFIG_SRC" -type f -print0 | sort -z)

# The shipped hyprland/execs.lua already starts realmheart.service on
# hyprland.start. Install the portable unit before exposing the Lua entrypoint,
# then refresh the current user manager when it is safe to do so.
install_realmheart_service
reload_current_user_manager

install_config_source "$CONFIG_SRC/hypr/hyprland.lua"

echo ""
echo "Done. Realmheart will start through realmheart.service on the next Hyprland login."
echo "Reload Hyprland config with: hyprctl reload"
