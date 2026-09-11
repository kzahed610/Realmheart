#!/usr/bin/env bash
set -euo pipefail

# Development/runtime bootstrap for realmheart-eventd.
#
# CMake invokes this after realmheart-eventd is relinked so the freshly-built
# daemon becomes the user-session service automatically. The helper is
# intentionally best-effort outside a live systemd user session (CI, staged
# packaging, containers): in those environments it prints why it skipped and
# exits successfully rather than turning a valid compile into a service-manager
# failure.

binary="${1:-}"
if [[ -z "$binary" ]]; then
    echo "realmheart-eventd-autostart: missing daemon binary path" >&2
    exit 2
fi
if [[ "${REALMHEART_EVENTD_AUTOSTART_DISABLE:-0}" == "1" ]]; then
    echo "realmheart-eventd-autostart: disabled by environment"
    exit 0
fi
if [[ ! -x "$binary" ]]; then
    echo "realmheart-eventd-autostart: daemon is not executable: $binary" >&2
    exit 2
fi
if [[ -z "${HOME:-}" ]]; then
    echo "realmheart-eventd-autostart: HOME is unavailable; skipping user service setup"
    exit 0
fi
if ! command -v systemctl >/dev/null 2>&1; then
    echo "realmheart-eventd-autostart: systemctl unavailable; skipping user service setup"
    exit 0
fi
if ! systemctl --user show-environment >/dev/null 2>&1; then
    echo "realmheart-eventd-autostart: no live systemd user manager; service will be installed by the Realmheart installer/session"
    exit 0
fi

systemd_escape_exec_arg() {
    local value="$1"
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    value=${value//%/%%}
    printf '"%s"' "$value"
}

stop_manual_instances() {
    local expected="$1"
    local expected_real
    expected_real="$(readlink -f -- "$expected" 2>/dev/null || printf '%s' "$expected")"

    # A developer may still have the daemon running by hand from the previous
    # test cycle. If systemd does not own a running unit yet, gracefully retire
    # only processes whose argv[0] resolves to this exact build output before
    # handing ownership to the user service.
    local proc pid argv0 argv0_real
    for proc in /proc/[0-9]*/cmdline; do
        [[ -r "$proc" ]] || continue
        pid="${proc#/proc/}"
        pid="${pid%/cmdline}"
        [[ "$pid" != "$$" ]] || continue
        argv0=""
        IFS= read -r -d '' argv0 <"$proc" 2>/dev/null || true
        [[ -n "${argv0:-}" ]] || continue
        argv0_real="$(readlink -f -- "$argv0" 2>/dev/null || printf '%s' "$argv0")"
        if [[ "$argv0_real" == "$expected_real" ]]; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done

    # Give graceful SIGTERM shutdown enough time to unlink the runtime socket.
    for _ in {1..20}; do
        local found=0
        for proc in /proc/[0-9]*/cmdline; do
            [[ -r "$proc" ]] || continue
            argv0=""
            IFS= read -r -d '' argv0 <"$proc" 2>/dev/null || true
            [[ -n "${argv0:-}" ]] || continue
            argv0_real="$(readlink -f -- "$argv0" 2>/dev/null || printf '%s' "$argv0")"
            if [[ "$argv0_real" == "$expected_real" ]]; then
                found=1
                break
            fi
        done
        (( found == 0 )) && return 0
        sleep 0.05
    done
}

unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
unit_path="$unit_dir/realmheart-eventd.service"
mkdir -p -- "$unit_dir"

temporary="$(mktemp "${unit_path}.tmp.XXXXXX")"
trap 'rm -f -- "$temporary"' EXIT
escaped_binary="$(systemd_escape_exec_arg "$binary")"

cat >"$temporary" <<EOF_UNIT
[Unit]
Description=Realmheart Event Surface daemon

[Service]
Type=simple
ExecStart=$escaped_binary
Restart=on-failure
RestartSec=1

[Install]
WantedBy=default.target
EOF_UNIT
chmod 0644 "$temporary"

if [[ ! -f "$unit_path" ]] || ! cmp -s -- "$temporary" "$unit_path"; then
    mv -f -- "$temporary" "$unit_path"
    trap - EXIT
    echo "realmheart-eventd-autostart: installed $unit_path"
else
    rm -f -- "$temporary"
    trap - EXIT
fi

systemctl --user daemon-reload
systemctl --user enable realmheart-eventd.service >/dev/null

if systemctl --user is-active --quiet realmheart-eventd.service; then
    systemctl --user restart realmheart-eventd.service
    echo "realmheart-eventd-autostart: restarted freshly-built daemon"
else
    stop_manual_instances "$binary"
    systemctl --user start realmheart-eventd.service
    echo "realmheart-eventd-autostart: started daemon in the background"
fi
