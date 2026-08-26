#!/usr/bin/env bash
for app in "$@"; do
    if command -v "$app" >/dev/null 2>&1; then
        exec "$app"
    elif [[ "$app" == *\ * ]]; then
        cmd=$(echo "$app" | awk '{print $1}')
        if command -v "$cmd" >/dev/null 2>&1; then
            exec $app
        fi
    fi
done
