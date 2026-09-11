#!/usr/bin/env bash
set -euo pipefail

REPO="${1:?repository root is required}"
TEMP_ROOT="$(mktemp -d -t realmheart-eventd-autostart-test-XXXXXX)"
trap 'rm -rf -- "$TEMP_ROOT"' EXIT

HOME_DIR="$TEMP_ROOT/home"
FAKE_BIN="$TEMP_ROOT/fake-bin"
STATE_DIR="$TEMP_ROOT/state"
EVENTD_DIR="$TEMP_ROOT/My Realmheart/build-hybrid"
EVENTD="$EVENTD_DIR/realmheart-eventd"
mkdir -p "$HOME_DIR" "$FAKE_BIN" "$STATE_DIR" "$EVENTD_DIR"
cp -- "$(command -v sleep)" "$EVENTD"
chmod +x "$EVENTD"

cat >"$FAKE_BIN/systemctl" <<'EOF_SYSTEMCTL'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"${RH_TEST_SYSTEMCTL_LOG:?}"
args=("$@")
if [[ "${args[0]:-}" == "--user" ]]; then
    args=("${args[@]:1}")
fi
case "${args[0]:-}" in
    show-environment|daemon-reload|enable)
        exit 0
        ;;
    is-active)
        [[ -f "${RH_TEST_SYSTEMCTL_STATE:?}/active" ]]
        ;;
    start|restart)
        : >"${RH_TEST_SYSTEMCTL_STATE:?}/active"
        exit 0
        ;;
    *)
        echo "unexpected fake systemctl call: ${args[*]}" >&2
        exit 3
        ;;
esac
EOF_SYSTEMCTL
chmod +x "$FAKE_BIN/systemctl"

export HOME="$HOME_DIR"
export XDG_CONFIG_HOME="$HOME_DIR/.config"
export PATH="$FAKE_BIN:/usr/bin:/bin"
export RH_TEST_SYSTEMCTL_LOG="$TEMP_ROOT/systemctl.log"
export RH_TEST_SYSTEMCTL_STATE="$STATE_DIR"

# Simulate the common development transition where eventd was started manually
# during testing before the build began managing it through systemd.
"$EVENTD" 60 &
MANUAL_PID=$!
sleep 0.05

"$REPO/tools/realmheart-eventd-autostart.sh" "$EVENTD" >/dev/null
if kill -0 "$MANUAL_PID" 2>/dev/null; then
    echo "eventd autostart helper did not retire the manual daemon instance" >&2
    kill -KILL "$MANUAL_PID" 2>/dev/null || true
    exit 1
fi
wait "$MANUAL_PID" 2>/dev/null || true
UNIT="$HOME_DIR/.config/systemd/user/realmheart-eventd.service"
[[ -f "$UNIT" ]] || {
    echo "eventd autostart helper did not install a user unit" >&2
    exit 1
}
grep -Fqx 'WantedBy=default.target' "$UNIT"
grep -Fqx "ExecStart=\"$EVENTD\"" "$UNIT"
grep -Fqx -- '--user daemon-reload' "$RH_TEST_SYSTEMCTL_LOG"
grep -Fqx -- '--user enable realmheart-eventd.service' "$RH_TEST_SYSTEMCTL_LOG"
grep -Fqx -- '--user start realmheart-eventd.service' "$RH_TEST_SYSTEMCTL_LOG"

: >"$RH_TEST_SYSTEMCTL_LOG"
"$REPO/tools/realmheart-eventd-autostart.sh" "$EVENTD" >/dev/null
grep -Fqx -- '--user restart realmheart-eventd.service' "$RH_TEST_SYSTEMCTL_LOG"

printf 'Realmheart eventd build-autostart service contract passed.\n'
