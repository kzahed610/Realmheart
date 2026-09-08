#!/usr/bin/env bash
set -euo pipefail

REPO="${1:?repository root is required}"
LOADER="$REPO/config/bin/realmheart-fx-load"
EXPECTED_COMMIT="efb50993780079460b0cbed1363e2166a2de1d9f"
EXPECTED_ABI="${EXPECTED_COMMIT}_aq_0.14_hu_0.14_hg_0.5_hc_0.1_hlg_0.6"
TEST_ROOT="$(mktemp -d -t realmheart-fx-loader-test-XXXXXX)"
trap 'rm -rf "$TEST_ROOT"' EXIT

FAKE_HYPRCTL="$TEST_ROOT/fake-hyprctl"
PLUGIN_SOURCE="$TEST_ROOT/realmheart-fx.so"
printf 'test plugin payload\n' > "$PLUGIN_SOURCE"

cat > "$FAKE_HYPRCTL" <<'FAKE_HYPRCTL_EOF'
#!/usr/bin/env bash
set -euo pipefail

case "${1:-} ${2:-}" in
    "plugin list")
        if [[ -f "${FAKE_STATE:?}" ]]; then
            printf 'Plugin: Realmheart-FX\nPath: %s\n' "${FAKE_RUNTIME_PLUGIN:-unknown}"
        else
            printf 'no plugins loaded\n'
        fi
        ;;
    "version -j")
        printf '{"commit":"%s","dirty":%s,"abiHash":"%s"}\n' \
            "${FAKE_COMMIT:?}" "${FAKE_DIRTY:?}" "${FAKE_ABI:?}"
        ;;
    "plugin load")
        printf 'load %s\n' "${3:?}" >> "${FAKE_CALL_LOG:?}"
        case "${FAKE_LOAD_BEHAVIOR:-success}" in
            fail)
                printf 'plugin crashed/threw in main: simulated init failure\n' >&2
                exit 42
                ;;
            no-confirm)
                printf 'plugin load command accepted\n'
                ;;
            success)
                printf '%s\n' "${3:?}" > "${FAKE_RUNTIME_PLUGIN:?}"
                touch "${FAKE_STATE:?}"
                printf 'plugin load command accepted\n'
                ;;
            *)
                printf 'unknown fake load behavior\n' >&2
                exit 43
                ;;
        esac
        ;;
    *)
        printf 'unexpected fake hyprctl call: %s\n' "$*" >&2
        exit 44
        ;;
esac
FAKE_HYPRCTL_EOF
chmod 755 "$FAKE_HYPRCTL"

run_loader() {
    local case_root="$1"
    shift
    set +e
    LOADER_OUTPUT="$(
        env \
            REALMHEART_FX_HYPRCTL="$FAKE_HYPRCTL" \
            REALMHEART_FX_SO="$PLUGIN_SOURCE" \
            REALMHEART_FX_RUNTIME_DIR="$case_root/runtime" \
            FAKE_STATE="$case_root/state" \
            FAKE_CALL_LOG="$case_root/calls.log" \
            FAKE_RUNTIME_PLUGIN="$case_root/fake-loaded-plugin" \
            FAKE_COMMIT="$EXPECTED_COMMIT" \
            FAKE_ABI="$EXPECTED_ABI" \
            FAKE_DIRTY=false \
            "$@" \
            "$LOADER" 2>&1
    )"
    LOADER_STATUS=$?
    set -e
}

assert_status() {
    local expected="$1"
    if [[ "$LOADER_STATUS" -ne "$expected" ]]; then
        printf 'expected loader status %s, got %s:\n%s\n' \
            "$expected" "$LOADER_STATUS" "$LOADER_OUTPUT" >&2
        exit 1
    fi
}

assert_output_contains() {
    local needle="$1"
    grep -Fq "$needle" <<<"$LOADER_OUTPUT" || {
        printf 'loader output did not contain %q:\n%s\n' "$needle" "$LOADER_OUTPUT" >&2
        exit 1
    }
}

# Clean runtime: the loader must preflight, copy to a unique private inode,
# load it, and verify the post-load plugin list. A second invocation must be
# idempotent and must not create another load request.
CLEAN_ROOT="$TEST_ROOT/clean"
mkdir -p "$CLEAN_ROOT"
run_loader "$CLEAN_ROOT" env FAKE_LOAD_BEHAVIOR=success
assert_status 0
assert_output_contains 'loaded '
[[ "$(stat -c '%a' "$CLEAN_ROOT/runtime")" == 700 ]]
RUNTIME_COPIES=("$CLEAN_ROOT"/runtime/realmheart-fx.*.so)
[[ "${#RUNTIME_COPIES[@]}" -eq 1 ]]
[[ "$(stat -c '%a' "${RUNTIME_COPIES[0]}")" == 755 ]]
[[ "$(wc -l < "$CLEAN_ROOT/calls.log")" -eq 1 ]]
run_loader "$CLEAN_ROOT" env FAKE_LOAD_BEHAVIOR=success
assert_status 0
assert_output_contains 'already loaded'
[[ "$(wc -l < "$CLEAN_ROOT/calls.log")" -eq 1 ]]

# A dirty runtime is rejected before a plugin copy or load request.
DIRTY_ROOT="$TEST_ROOT/dirty"
mkdir -p "$DIRTY_ROOT"
run_loader "$DIRTY_ROOT" env FAKE_DIRTY=true FAKE_LOAD_BEHAVIOR=success
assert_status 1
assert_output_contains 'reports a dirty build'
[[ ! -e "$DIRTY_ROOT/calls.log" ]]

# A commit mismatch is rejected before loading.
HASH_ROOT="$TEST_ROOT/hash-mismatch"
mkdir -p "$HASH_ROOT"
run_loader "$HASH_ROOT" env FAKE_COMMIT=0000000000000000000000000000000000000000 FAKE_LOAD_BEHAVIOR=success
assert_status 1
assert_output_contains 'commit mismatch'
[[ ! -e "$HASH_ROOT/calls.log" ]]

# A plugin-init exception is surfaced and remains a failure.
INIT_ROOT="$TEST_ROOT/init-failure"
mkdir -p "$INIT_ROOT"
run_loader "$INIT_ROOT" env FAKE_LOAD_BEHAVIOR=fail
assert_status 1
assert_output_contains 'simulated init failure'
assert_output_contains 'plugin initialization/load failed'
[[ "$(wc -l < "$INIT_ROOT/calls.log")" -eq 1 ]]

# A successful hyprctl load without list confirmation is not accepted.
CONFIRM_ROOT="$TEST_ROOT/list-confirmation"
mkdir -p "$CONFIRM_ROOT"
run_loader "$CONFIRM_ROOT" env FAKE_LOAD_BEHAVIOR=no-confirm
assert_status 1
assert_output_contains 'did not confirm Realmheart-FX'
[[ "$(wc -l < "$CONFIRM_ROOT/calls.log")" -eq 1 ]]

printf 'Realmheart-FX loader preflight, failure, runtime-copy, and confirmation contracts passed.\n'
