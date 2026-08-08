#pragma once

#include <optional>
#include <string_view>

namespace realmheart::core {

enum class ShellCommand {
    ToggleRightSidebar,
    ToggleBar,
    ToggleCharacter,
    SetCharacterHairMode,
    ShowOSDVolume,
    ShowOSDBrightness,
    LockSession,
    OpenLogoutMenu,
    ScreenshotFull,
    ScreenshotArea,
    ExtractOCR,
    StartRecording,
    StopRecording,
    ToggleNotes,
    SetWallpaper,
    SetWallpaperPath,
    SetWallpaperBackend,
    GenerateTheme,
    LaunchLauncher,
    LaunchLauncherQuery,
    ToggleWorkspaceOverview,
    ToggleWorldscar,
    Restart,
    Quit,
};

std::optional<ShellCommand> parse_shell_command(std::string_view name);
std::string_view shell_action_name(ShellCommand command);
bool shell_command_requires_argument(ShellCommand command);

} // namespace realmheart::core
