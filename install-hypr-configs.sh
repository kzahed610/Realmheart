#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# Realmheart Hyprland Config Installer
# =============================================================================
# Copies all portable config files from ./config/ into ~/.config/hypr/ and
# ~/.config/realmheart/. Creates .bak backups with timestamps before
# overwriting. Safe to re-run — always backs up, never deletes.
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

echo "=== Realmheart Hyprland Config Installer ==="
echo ""

# Walk all files in config/ and copy to the right location
while IFS= read -r -d '' src; do
    # Compute relative path from config/
    rel="${src#$CONFIG_SRC/}"
    dest_base=""

    # Files under config/hypr/ go to ~/.config/hypr/
    if [[ "$rel" == hypr/* ]]; then
        dest_base="$HYPRLAND_DIR/${rel#hypr/}"
    elif [[ "$rel" == realmheart/* ]]; then
        dest_base="$REALMHEART_DIR/${rel#realmheart/}"
    elif [[ "$rel" == pam/* ]]; then
        dest_base="$HYPRLAND_DIR/${rel#pam/}"
    else
        continue
    fi

    echo "[$(basename "$dest_base")]"
    copy_file "$src" "$dest_base"
done < <(find "$CONFIG_SRC" -type f -print0)

echo ""
echo "Done. Reload Hyprland with: hyprctl reload"
