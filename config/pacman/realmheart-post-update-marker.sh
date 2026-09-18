#!/bin/sh
# Record that a package transaction finished so the Realmheart Doctor can
# correlate it at the next session or via `realmheart-doctor post-update`.
#
# Runs as root from a pacman hook: it stays tiny, never calls sudo, never
# repairs anything and never runs the Doctor inside the transaction.
set -eu

marker_dir=/run/realmheart
[ -d "$marker_dir" ] || mkdir -p "$marker_dir"
touch "$marker_dir/post-update.pending"
