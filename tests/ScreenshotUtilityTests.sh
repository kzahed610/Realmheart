#!/usr/bin/env bash
set -Eeuo pipefail

root="${1:?source root required}"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
bin="$tmp/bin"
mkdir -p "$bin"
printf 'png-fixture\n' > "$tmp/input.png"
mkdir -p "$tmp/home/.config/realmheart/scripts"
cp "$root/config/realmheart/scripts/search_image.sh" \
    "$tmp/home/.config/realmheart/scripts/search_image.sh"
chmod +x "$tmp/home/.config/realmheart/scripts/search_image.sh"

cat > "$bin/curl" <<'FAKECURL'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${REALMHEART_TEST_CURL_LOG:?}"
printf '%s' 'https://lens.google.test/result?id=fixture'
FAKECURL
cat > "$bin/xdg-open" <<'FAKEXDG'
#!/usr/bin/env bash
printf '%s\n' "$1" > "${REALMHEART_TEST_OPEN_LOG:?}"
exit "${REALMHEART_TEST_XDG_STATUS:-0}"
FAKEXDG
cat > "$bin/gio" <<'FAKEGIO'
#!/usr/bin/env bash
printf '%s\n' "$2" > "${REALMHEART_TEST_GIO_LOG:?}"
exit "${REALMHEART_TEST_GIO_STATUS:-0}"
FAKEGIO
cat > "$bin/slurp" <<'FAKESLURP'
#!/usr/bin/env bash
printf '%s' '0,0 10x10'
FAKESLURP
cat > "$bin/grim" <<'FAKEGRIM'
#!/usr/bin/env bash
printf '%s' 'png-fixture'
FAKEGRIM
chmod +x "$bin"/*

export REALMHEART_TEST_CURL_LOG="$tmp/curl.log"
export REALMHEART_TEST_OPEN_LOG="$tmp/open.log"
export REALMHEART_TEST_GIO_LOG="$tmp/gio.log"
export REALMHEART_LENS_UPLOAD_URL='https://lens.google.test/v3/upload'
export HOME="$tmp/home"
export PATH="$bin:/usr/bin:/bin"

"$root/config/realmheart/scripts/search_image.sh" "$tmp/input.png"
opened="$(<"$tmp/open.log")"
[[ "$opened" == https://lens.google.test/result\?id=fixture ]]
! grep -Fq 'file://' "$tmp/open.log"
! grep -Fq "$tmp/input.png" "$tmp/open.log"
grep -Fq -- '--form encoded_image=@' "$tmp/curl.log"

export REALMHEART_TEST_XDG_STATUS=7
export REALMHEART_TEST_GIO_STATUS=9
if "$root/config/realmheart/scripts/search_image.sh" "$tmp/input.png"; then
    printf '%s\n' 'expected opener failure to propagate' >&2
    exit 1
fi

unset REALMHEART_TEST_XDG_STATUS REALMHEART_TEST_GIO_STATUS
"$root/config/hypr/hyprland/scripts/snip_to_search.sh"

cat > "$bin/slurp" <<'FAILSLURP'
#!/usr/bin/env bash
exit 7
FAILSLURP
chmod +x "$bin/slurp"
if "$root/config/hypr/hyprland/scripts/snip_to_search.sh"; then
    printf '%s\n' 'expected slurp failure to propagate' >&2
    exit 1
fi
