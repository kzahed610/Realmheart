#!/usr/bin/env fish

function usage
    echo "Usage: tools/workspace-morph-profile.fish [--cycles N] [--rapid N]"
    echo "Runs normal open/close cycles, then optional rapid reversal toggles."
end

function display_value --argument-names value
    if test -n "$value"
        echo $value
    else
        echo unknown
    end
end

argparse 'h/help' 'c/cycles=' 'r/rapid=' -- $argv
or begin
    usage
    exit 2
end

if set -q _flag_help
    usage
    exit 0
end

set cycles 30
set rapid 20
if set -q _flag_cycles
    set cycles $_flag_cycles
end
if set -q _flag_rapid
    set rapid $_flag_rapid
end

if not string match -rq '^[0-9]+$' -- $cycles
    echo "--cycles must be a non-negative integer" >&2
    exit 2
end
if not string match -rq '^[0-9]+$' -- $rapid
    echo "--rapid must be a non-negative integer" >&2
    exit 2
end

if test -x ./build-hybrid/realmheart
    set realmheart ./build-hybrid/realmheart
else if type -q realmheart
    set realmheart (command -s realmheart)
else
    echo "Realmheart executable not found; build build-hybrid/realmheart first." >&2
    exit 1
end

set pid (systemctl --user show realmheart.service --property MainPID --value 2>/dev/null)
if test -z "$pid"; or test "$pid" = 0; or not test -r /proc/$pid/status
    echo "realmheart.service is not running." >&2
    exit 1
end

function rss_kib --argument-names target_pid
    awk '/^VmRSS:/ { print $2; exit }' /proc/$target_pid/status 2>/dev/null
end

set started_at (date --iso-8601=seconds)
set rss_before (rss_kib $pid)
set rss_peak $rss_before

echo "Realmheart PID: $pid"
echo "Starting RSS: "(display_value "$rss_before")" KiB"
echo "Normal cycles: $cycles"

for index in (seq $cycles)
    $realmheart --command workspace-overview-toggle >/dev/null
    sleep 0.62
    $realmheart --command workspace-overview-toggle >/dev/null
    sleep 0.52

    set current_rss (rss_kib $pid)
    if test -n "$current_rss"
        if test -z "$rss_peak"; or test $current_rss -gt $rss_peak
            set rss_peak $current_rss
        end
    end
end

if test $rapid -gt 0
    echo "Rapid reversal toggles: $rapid"
    for index in (seq $rapid)
        $realmheart --command workspace-overview-toggle >/dev/null
        sleep 0.08
    end
    sleep 0.55
    if test (math "$rapid % 2") -eq 1
        $realmheart --command workspace-overview-toggle >/dev/null
        sleep 0.55
    end
end

sleep 1
set rss_after (rss_kib $pid)

echo
printf "RSS before: %s KiB\n" (display_value "$rss_before")
printf "RSS peak:   %s KiB\n" (display_value "$rss_peak")
printf "RSS after:  %s KiB\n" (display_value "$rss_after")
if test -n "$rss_before"; and test -n "$rss_after"
    printf "RSS delta:  %d KiB\n" (math "$rss_after - $rss_before")
end

echo
echo "Workspace morph diagnostics since $started_at:"
journalctl --user -u realmheart.service --since "$started_at" --no-pager \
    | string match -r 'Realmheart workspace morph.*(endpoint=|fallback|Captured|First shader frame)'
or true
