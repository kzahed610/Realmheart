#include "core/ShellCommand.hpp"

namespace realmheart::core {

std::optional<ShellCommand> parse_shell_command(std::string_view name) {
    if (name == "sidebar-right-toggle") return ShellCommand::ToggleRightSidebar;
    if (name == "bar-toggle") return ShellCommand::ToggleBar;
    if (name == "osd-volume") return ShellCommand::ShowOSDVolume;
    if (name == "osd-brightness") return ShellCommand::ShowOSDBrightness;
    if (name == "lock-session") return ShellCommand::LockSession;
    if (name == "logout-menu") return ShellCommand::OpenLogoutMenu;
    if (name == "screenshot-full") return ShellCommand::ScreenshotFull;
    if (name == "screenshot-area") return ShellCommand::ScreenshotArea;
    if (name == "extract-ocr") return ShellCommand::ExtractOCR;
    if (name == "start-recording") return ShellCommand::StartRecording;
    if (name == "stop-recording") return ShellCommand::StopRecording;
    if (name == "toggle-notes") return ShellCommand::ToggleNotes;
    if (name == "set-wallpaper") return ShellCommand::SetWallpaper;
    if (name == "set-wallpaper-path") return ShellCommand::SetWallpaperPath;
    if (name == "set-wallpaper-backend") return ShellCommand::SetWallpaperBackend;
    if (name == "generate-theme") return ShellCommand::GenerateTheme;
    if (name == "launch-launcher") return ShellCommand::LaunchLauncher;
    if (name == "quit") return ShellCommand::Quit;
    return std::nullopt;
}

std::string_view shell_action_name(ShellCommand command) {
    switch (command) {
    case ShellCommand::ToggleRightSidebar: return "sidebar-right-toggle";
    case ShellCommand::ToggleBar: return "bar-toggle";
    case ShellCommand::ShowOSDVolume: return "osd-volume";
    case ShellCommand::ShowOSDBrightness: return "osd-brightness";
    case ShellCommand::LockSession: return "lock-session";
    case ShellCommand::OpenLogoutMenu: return "logout-menu";
    case ShellCommand::ScreenshotFull: return "screenshot-full";
    case ShellCommand::ScreenshotArea: return "screenshot-area";
    case ShellCommand::ExtractOCR: return "extract-ocr";
    case ShellCommand::StartRecording: return "start-recording";
    case ShellCommand::StopRecording: return "stop-recording";
    case ShellCommand::ToggleNotes: return "toggle-notes";
    case ShellCommand::SetWallpaper: return "set-wallpaper";
    case ShellCommand::SetWallpaperPath: return "set-wallpaper-path";
    case ShellCommand::SetWallpaperBackend: return "set-wallpaper-backend";
    case ShellCommand::GenerateTheme: return "generate-theme";
    case ShellCommand::LaunchLauncher: return "launch-launcher";
    case ShellCommand::Quit: return "quit";
    }
    return {};
}

bool shell_command_requires_argument(ShellCommand command) {
    return command == ShellCommand::SetWallpaperPath ||
           command == ShellCommand::SetWallpaperBackend;
}

} // namespace realmheart::core
