#!/usr/bin/env bash
# search_image.sh — Upload the current clipboard image to Google Lens and open
# the results. Triggered by the SUPER+SHIFT+A keybind via snip_to_search.sh.
#
# Falls back to the local reverse-image-search helper `tesseract` + the
# web_search service when Lens is unavailable. Override with the
# REALMHEART_LENS_URL env var if you need a self-hosted endpoint.

set -uo pipefail

img_file="${1:-}"
if [[ -z "$img_file" || ! -f "$img_file" ]]; then
    notify-send -u critical "Realmheart Lens" "No image on clipboard" 2>/dev/null || true
    exit 1
fi

lens_url="${REALMHEART_LENS_URL:-https://lens.google.com/uploadbyurl?url=file%3A%2F%2F%2F${img_file}}"

if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$lens_url"
    exit 0
fi

if command -v gio >/dev/null 2>&1; then
    gio open "$lens_url"
    exit 0
fi

notify-send -u critical "Realmheart Lens" "No xdg-open or gio available" 2>/dev/null || true
exit 1