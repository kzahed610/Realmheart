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
    mkdir -p "$dest_dir" 2>/dev/null
    # If the directory doesn't exist yet or is writable, no sudo needed
    if [[ -d "$dest_dir" ]] && [[ -w "$dest_dir" ]]; then
        return 1   # false (no sudo)
    fi
    # If the file exists and is not writable, need sudo
    if [[ -f "$dest" ]] && [[ ! -w "$dest" ]]; then
        return 0  # true (sudo needed)
    fi
    return 1      # false (no sudo)
}

copy_file() {
    local src="$1"
    local dest="$2"
    local use_sudo=""

    if need_sudo "$dest"; then
        use_sudo="sudo"
    fi

    if [[ -f "$dest" ]]; then
        local bak="${dest}.bak.$(date +%Y%m%d_%H%M%S)"
        if [[ -n "$use_sudo" ]]; then
            sudo cp "$dest" "$bak"
        else
            cp "$dest" "$bak"
        fi
        echo "  backed up: $dest -> $bak"
    fi

    if [[ -n "$use_sudo" ]]; then
        sudo cp "$src" "$dest"
    else
        cp "$src" "$dest"
    fi
    echo "  installed: $dest"
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
        dest_base="$HYPRLAND_DIR/${rel#pam/}"
    else
        return 0
    fi

    echo "[$(basename "$dest_base")]"
    copy_file "$src" "$dest_base"
}

install_realmheart_service() {
    local binary="$SCRIPT_DIR/build-hybrid/realmheart"
    local destination="$SYSTEMD_USER_DIR/realmheart.service"
    local temporary
    temporary="$(mktemp -t realmheart-service-XXXXXX)"

    {
        printf '%s\n' \
            '[Unit]' \
            'Description=Realmheart desktop shell' \
            'After=graphical-session.target' \
            'PartOf=graphical-session.target' \
            '' \
            '[Service]' \
            'Type=simple' \
            "ExecStart=$binary --shell --wallpaper-backend native" \
            'Restart=on-failure' \
            'RestartSec=2' \
            '' \
            '[Install]' \
            'WantedBy=graphical-session.target'
    } >"$temporary"
    chmod 0644 "$temporary"

    echo "[realmheart.service]"
    if ! copy_file "$temporary" "$destination"; then
        rm -f "$temporary"
        return 1
    fi
    rm -f "$temporary"

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

# Install dependencies first. Hyprland watches its entrypoint and may reload as
# soon as hyprland.lua appears; copying it before realmheart_fx.lua exists causes
# a first-run-only `module 'realmheart_fx' not found` warning.
while IFS= read -r -d '' src; do
    rel="${src#$CONFIG_SRC/}"
    case "$rel" in
        hypr/hyprland.lua|hypr/hyprland.conf) continue ;;
    esac
    install_config_source "$src"
done < <(find "$CONFIG_SRC" -type f -print0 | sort -z)

# The shipped hyprland/execs.lua already starts realmheart.service on
# hyprland.start. Install the missing portable unit before exposing the Lua
# entrypoint, then refresh the current user manager when it is safe to do so.
install_realmheart_service
reload_current_user_manager

for entrypoint in hypr/hyprland.conf hypr/hyprland.lua; do
    install_config_source "$CONFIG_SRC/$entrypoint"
done

echo ""
echo "Done. Realmheart will start through realmheart.service on the next Hyprland login."
echo "Reload Hyprland config with: hyprctl reload"
