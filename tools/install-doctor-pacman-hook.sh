#!/usr/bin/env bash
# Install the Realmheart Doctor pacman post-transaction hook + marker script.
#
# The doctor's post-update correlation depends on a tiny pacman hook that drops
# /run/realmheart/post-update.pending after every package transaction. This
# script installs that hook and its marker helper into the system paths the
# rendered hook references (/usr/local/libexec + /etc/pacman.d/hooks).
#
# Run once, as root:
#     sudo bash tools/install-doctor-pacman-hook.sh
#
# Safe to re-run; it overwrites only the two files it owns.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK_SRC="$REPO/build-hybrid/pacman/10-realmheart-doctor.hook"
MARKER_SRC="$REPO/config/pacman/realmheart-post-update-marker.sh"
HOOK_DEST="/etc/pacman.d/hooks/10-realmheart-doctor.hook"
MARKER_DEST="/usr/local/libexec/realmheart/realmheart-post-update-marker"

if [[ $EUID -ne 0 ]]; then
    echo "error: must run as root (try: sudo bash $0)" >&2
    exit 1
fi

if [[ ! -f "$HOOK_SRC" ]]; then
    echo "error: rendered hook not found: $HOOK_SRC" >&2
    echo "       build first: cmake --build build-hybrid" >&2
    exit 1
fi

install -d -m 0755 /usr/local/libexec/realmheart
install -D -m 0755 "$MARKER_SRC" "$MARKER_DEST"
install -D -m 0644 "$HOOK_SRC" "$HOOK_DEST"

echo "installed:"
echo "  hook:   $HOOK_DEST"
echo "  marker: $MARKER_DEST"
echo
echo "verify: pacman will now drop /run/realmheart/post-update.pending after"
echo "        every transaction; realmheart-doctor post-update correlates it."