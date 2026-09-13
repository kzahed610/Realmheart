-- Realmheart-owned session startup. Personal desktop services belong in
-- ~/.config/hypr/custom/execs.lua, which the installer preserves across upgrades.
hl.on("hyprland.start", function ()
    -- Import the compositor/session identity used by user services and portals.
    hl.exec_cmd("dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP HYPRLAND_INSTANCE_SIGNATURE")

    -- Realmheart shell + clipboard history + Hyprland portal.
    hl.exec_cmd("systemctl --user start realmheart.service realmheart-cliphist-text.service realmheart-cliphist-image.service xdg-desktop-portal-hyprland.service")

    -- User-island hook. This script is preserved/replaceable under custom/.
    hl.exec_cmd("$HOME/.config/hypr/custom/scripts/__restore_video_wallpaper.sh")

    -- Realmheart session integration.
    hl.exec_cmd("hypridle")
end)
