# Realmheart Starship integration.
# This drop-in avoids replacing the user's personal config.fish.
if status is-interactive
    set -l __realmheart_state_home "$HOME/.local/state"
    if set -q XDG_STATE_HOME; and test -n "$XDG_STATE_HOME"
        set __realmheart_state_home "$XDG_STATE_HOME"
    end
    set -gx STARSHIP_CONFIG "$__realmheart_state_home/realmheart/theme/starship.toml"
    if type -q starship
        starship init fish | source
        if functions -q enable_transience
            enable_transience
        end
    end
end
