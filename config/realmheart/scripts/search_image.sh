#!/usr/bin/env bash
# search_image.sh — Upload a PNG to Google Lens and open the results.
# Triggered by the SUPER+SHIFT+A keybind via snip_to_search.sh.
#
# The image is sent directly to Google's multipart upload endpoint. Override
# REALMHEART_LENS_UPLOAD_URL for a compatible self-hosted endpoint.

set -uo pipefail

img_file="${1:-}"
if [[ -z "$img_file" || ! -f "$img_file" || ! -s "$img_file" ]]; then
    notify-send -u critical "Realmheart Lens" "No captured image was provided" 2>/dev/null || true
    exit 1
fi

upload_url="${REALMHEART_LENS_UPLOAD_URL:-https://lens.google.com/v3/upload}"
if ! command -v curl >/dev/null 2>&1; then
    notify-send -u critical "Realmheart Lens" "curl is required for image upload" 2>/dev/null || true
    exit 1
fi

if ! lens_url="$(curl \
        --fail --silent --show-error --location \
        --max-time 30 \
        --output /dev/null \
        --write-out '%{url_effective}' \
        --form "encoded_image=@${img_file};type=image/png" \
        "$upload_url")"; then
    notify-send -u critical "Realmheart Lens" "Image upload failed" 2>/dev/null || true
    exit 1
fi
if [[ "$lens_url" != https://* && "$lens_url" != http://* ]]; then
    notify-send -u critical "Realmheart Lens" "Lens returned no usable result URL" 2>/dev/null || true
    exit 1
fi

if command -v xdg-open >/dev/null 2>&1; then
    if xdg-open "$lens_url"; then
        exit 0
    fi
fi

if command -v gio >/dev/null 2>&1; then
    if gio open "$lens_url"; then
        exit 0
    fi
fi

notify-send -u critical "Realmheart Lens" "Unable to open Lens results" 2>/dev/null || true
exit 1