# ── Realmheart terminal integration · Relic Grimoire v4.2 N1 ─────────────
# Fish owns cheap, event-driven prompt state; Starship only renders cached
# environment values. Location work happens on PWD changes, not every prompt.

set -l __realmheart_state_home "$HOME/.local/state"
if set -q XDG_STATE_HOME; and test -n "$XDG_STATE_HOME"
    set __realmheart_state_home "$XDG_STATE_HOME"
end
set -g __realmheart_theme_dir "$__realmheart_state_home/realmheart/theme"

function __realmheart_apply_generated_theme
    if test -f "$__realmheart_theme_dir/fish-theme.fish"
        source "$__realmheart_theme_dir/fish-theme.fish"
    end

    # Repaint active command lines after a live wallpaper/theme change.
    if status is-interactive
        commandline -f repaint >/dev/null 2>&1
    end
end

# Fish 4.3+ keeps user-facing color vars global per shell. A universal epoch is
# used only as a cross-session notification bus: every running Fish instance
# re-sources the newly generated Matugen-derived theme when this value changes.
function __realmheart_theme_changed --on-variable=__realmheart_theme_epoch
    __realmheart_apply_generated_theme
end

# Root remains the one ceremonial invariant: gold crown instead of wallpaper
# dominant. Everything else in the visual system is Matugen-derived.
function __realmheart_sync_identity
    if test "$EUID" -eq 0
        set -gx RH_ROOT_SIGIL "󰕇"
        set -eg RH_USER_SIGIL
    else
        set -gx RH_USER_SIGIL "✶"
        set -eg RH_ROOT_SIGIL
    end
end

function __realmheart_repeat_glyph --argument-names glyph wanted
    set -l out ""
    set -l i 0
    while test $i -lt $wanted
        set out "$out$glyph"
        set i (math "$i + 1")
    end
    printf '%s' "$out"
end

# N1 Depth Compass. Depth is measured relative to repo root when inside a git
# repository, otherwise relative to HOME (or / outside HOME). Five cells are
# enough for peripheral awareness; deeper trees saturate rather than expanding.
function __realmheart_set_depth --argument-names raw_depth
    set -eg RH_DEPTH_ON RH_DEPTH_HOT RH_DEPTH_OFF

    set -l depth $raw_depth
    if not string match -qr '^[0-9]+$' -- "$depth"
        set depth 0
    end
    if test $depth -gt 5
        set depth 5
    end

    if test $depth -le 0
        set -gx RH_DEPTH_OFF (__realmheart_repeat_glyph "▫" 5)
        return
    end

    set -l completed (math "$depth - 1")
    set -l remaining (math "5 - $depth")
    if test $completed -gt 0
        set -gx RH_DEPTH_ON (__realmheart_repeat_glyph "▪" $completed)
    end
    set -gx RH_DEPTH_HOT "▪"
    if test $remaining -gt 0
        set -gx RH_DEPTH_OFF (__realmheart_repeat_glyph "▫" $remaining)
    end
end

function __realmheart_clear_ribbon
    set -eg RH_RIBBON_ROOT_ONLY RH_RIBBON_SCOPE
    set -eg RH_RIBBON_P1 RH_RIBBON_P2 RH_RIBBON_P3 RH_RIBBON_P4
    set -eg RH_RIBBON_CURRENT_FROM_SCOPE RH_RIBBON_CURRENT_FROM_A RH_RIBBON_CURRENT_FROM_B
end

# N1 Aether Ribbon. Git is consulted only when PWD changes. The result is split
# into fixed exported cells so Starship can style each material segment without
# running a custom command on every prompt.
function __realmheart_sync_location --on-variable=PWD
    __realmheart_clear_ribbon

    if test "$PWD" = "$HOME"
        set -gx RH_CWD_NAME "~"
    else if test "$PWD" = "/"
        set -gx RH_CWD_NAME "/"
    else
        set -gx RH_CWD_NAME (path basename -- "$PWD")
    end

    set -l scope ""
    set -l relative ""
    set -l repo_root (command git -C "$PWD" rev-parse --show-toplevel 2>/dev/null)
    set -l repo_status $status

    if test $repo_status -eq 0; and test -n "$repo_root"
        set -l repo_name (path basename -- "$repo_root")
        if test "$PWD" = "$repo_root"
            set -gx RH_RIBBON_ROOT_ONLY "󰘿 $repo_name"
            __realmheart_set_depth 0
            return
        end
        set scope "󰘿 $repo_name"
        set relative (string replace -- "$repo_root/" "" "$PWD")
    else if test "$PWD" = "$HOME"
        set -gx RH_RIBBON_ROOT_ONLY "󰉋 ~"
        __realmheart_set_depth 0
        return
    else if test "$PWD" = "/"
        set -gx RH_RIBBON_ROOT_ONLY "󰉋 /"
        __realmheart_set_depth 0
        return
    else if string match -q -- "$HOME/*" "$PWD"
        set scope "~"
        set relative (string replace -- "$HOME/" "" "$PWD")
    else
        set scope "/"
        set relative (string trim -l -c / -- "$PWD")
    end

    set -l segments (string split / -- "$relative")
    set -l segment_count (count $segments)
    if test $segment_count -le 0
        set -gx RH_RIBBON_ROOT_ONLY "󰉋 $RH_CWD_NAME"
        __realmheart_set_depth 0
        return
    end

    set -l current $segments[-1]
    set -l parents
    if test $segment_count -gt 1
        set parents $segments[1..-2]
    end

    # The ribbon has four parent slots. Deep trees retain the three nearest
    # parents and collapse older ancestry into an explicit …+N cell; the Depth
    # Compass still preserves the sense of actual nesting.
    set -l shown_parents
    set -l parent_count (count $parents)
    if test $parent_count -gt 4
        set -l hidden_count (math "$parent_count - 3")
        set shown_parents "…+$hidden_count" $parents[-3..-1]
    else
        set shown_parents $parents
    end

    set -gx RH_RIBBON_SCOPE "$scope"
    set -l idx 1
    for segment in $shown_parents
        set -gx "RH_RIBBON_P$idx" "$segment"
        set idx (math "$idx + 1")
    end

    set -l current_label "󰉋 $current"
    set -l visible_parent_count (count $shown_parents)
    if test $visible_parent_count -eq 0
        set -gx RH_RIBBON_CURRENT_FROM_SCOPE "$current_label"
    else if test (math "$visible_parent_count % 2") -eq 1
        set -gx RH_RIBBON_CURRENT_FROM_A "$current_label"
    else
        set -gx RH_RIBBON_CURRENT_FROM_B "$current_label"
    end

    __realmheart_set_depth $segment_count
end

# Cache a short host label once. Context changes later (venv activation, etc.)
# do not need to spawn hostname repeatedly.
set -g __realmheart_host_short (command hostname 2>/dev/null)
if test -n "$__realmheart_host_short"
    set __realmheart_host_short (string replace -r '\..*$' '' -- "$__realmheart_host_short")
else
    set __realmheart_host_short "remote"
end

# N1 Context Cartouche. No LOCAL chip: ordinary state stays visually quiet.
# SSH, environment/runtime and container state appear only when meaningful.
function __realmheart_sync_context
    set -eg RH_CTX_REMOTE RH_CTX_ENV RH_CTX_CONTAINER

    if set -q SSH_CONNECTION; and test -n "$SSH_CONNECTION"
        set -gx RH_CTX_REMOTE "SSH $__realmheart_host_short"
    else if set -q SSH_TTY; and test -n "$SSH_TTY"
        set -gx RH_CTX_REMOTE "SSH $__realmheart_host_short"
    end

    if set -q VIRTUAL_ENV; and test -n "$VIRTUAL_ENV"
        set -gx RH_CTX_ENV "PY "(path basename -- "$VIRTUAL_ENV")
    else if set -q CONDA_DEFAULT_ENV; and test -n "$CONDA_DEFAULT_ENV"
        set -gx RH_CTX_ENV "CONDA $CONDA_DEFAULT_ENV"
    else if set -q IN_NIX_SHELL; and test -n "$IN_NIX_SHELL"
        set -gx RH_CTX_ENV "NIX $IN_NIX_SHELL"
    end

    if set -q container; and test -n "$container"
        set -gx RH_CTX_CONTAINER "CTR $container"
    else if test -e /.dockerenv
        set -gx RH_CTX_CONTAINER "CTR docker"
    end
end

# Runtime contexts can change inside a live shell, so refresh only on the
# variables activation scripts actually mutate.
function __realmheart_virtualenv_changed --on-variable=VIRTUAL_ENV
    __realmheart_sync_context
end
function __realmheart_conda_changed --on-variable=CONDA_DEFAULT_ENV
    __realmheart_sync_context
end
function __realmheart_nix_changed --on-variable=IN_NIX_SHELL
    __realmheart_sync_context
end
function __realmheart_container_changed --on-variable=container
    __realmheart_sync_context
end

function __realmheart_format_duration
    set -l ms $argv[1]
    if not string match -qr '^[0-9]+$' -- "$ms"
        return 1
    end
    if test "$ms" -lt 1000
        return 1
    end

    set -l seconds (math "floor(($ms + 500) / 1000)")
    if test "$seconds" -lt 60
        printf '%ss' "$seconds"
    else
        set -l minutes (math "floor($seconds / 60)")
        set -l remainder (math "$seconds % 60")
        printf '%dm %02ds' "$minutes" "$remainder"
    end
end

# Capture duration without allowing helper logic to erase the real exit status.
function __realmheart_capture_command_meta --on-event fish_postexec
    set -l rh_status $status
    set -l rh_duration $CMD_DURATION

    set -eg RH_DURATION_OK RH_DURATION_ERR
    set -l rendered (__realmheart_format_duration "$rh_duration")
    set -l rendered_status $status
    if test $rendered_status -eq 0; and test -n "$rendered"
        if test "$rh_status" -eq 0
            set -gx RH_DURATION_OK "$rendered"
        else
            set -gx RH_DURATION_ERR "$rendered"
        end
    end

    return "$rh_status"
end

function __realmheart_capture_syntax_error --on-event fish_posterror
    set -l rh_status $status
    set -eg RH_DURATION_OK RH_DURATION_ERR
    return "$rh_status"
end

# Kitty title is a second, compact location cue.
function fish_title
    set -l rh_cmd (status current-command)
    if test "$rh_cmd" = fish
        printf '󰉋 %s' "$RH_CWD_NAME"
    else
        printf '%s · 󰉋 %s' "$rh_cmd" "$RH_CWD_NAME"
    end
end

# Do not inherit stale command metadata into nested Fish sessions.
set -eg RH_DURATION_OK RH_DURATION_ERR
__realmheart_sync_identity
__realmheart_sync_location
__realmheart_sync_context
__realmheart_apply_generated_theme
