-- Load the Realmheart FX Hyprland plugin after compositor IPC is ready.
hl.on("hyprland.start", function()
    hl.exec_cmd("sleep 0.5 && $HOME/.local/bin/realmheart-fx-load")
end)

return {}
