#!/usr/bin/env bash
grim -g "$(slurp -b 00000080)" - | wl-copy && ~/.config/realmheart/scripts/search_image.sh
