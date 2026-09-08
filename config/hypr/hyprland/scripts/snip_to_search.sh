#!/usr/bin/env bash
set -Eeuo pipefail

selection="$(slurp -b 00000080)" || {
    printf '%s\n' "Realmheart Lens: selection cancelled or unavailable" >&2
    exit 1
}
if [[ -z "$selection" ]]; then
    printf '%s\n' "Realmheart Lens: empty selection" >&2
    exit 1
fi

image_file="$(mktemp "${XDG_RUNTIME_DIR:-/tmp}/realmheart-lens.XXXXXX.png")"
trap 'rm -f -- "$image_file"' EXIT

if ! grim -g "$selection" - >"$image_file"; then
    printf '%s\n' "Realmheart Lens: screenshot capture failed" >&2
    exit 1
fi
if [[ ! -s "$image_file" ]]; then
    printf '%s\n' "Realmheart Lens: screenshot capture was empty" >&2
    exit 1
fi

if "$HOME/.config/realmheart/scripts/search_image.sh" "$image_file"; then
    exit 0
else
    status=$?
    exit "$status"
fi
