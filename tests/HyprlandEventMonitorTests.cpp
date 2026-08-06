#include "services/HyprlandEventMonitor.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_workspace_event_filter() {
    using realmheart::services::HyprlandEventMonitor;
    require(HyprlandEventMonitor::is_workspace_event("workspacev2>>2,code"),
            "workspace changes must trigger refresh");
    require(HyprlandEventMonitor::is_workspace_event("openwindow>>abc,2,kitty,title"),
            "window-count changes must trigger refresh");
    require(HyprlandEventMonitor::is_workspace_event("focusedmon>>DP-1,2"),
            "focused monitor changes must trigger refresh");
    require(HyprlandEventMonitor::is_workspace_event("windowtitlev2>>abc,Updated title"),
            "window title changes must refresh overview cards");
    require(!HyprlandEventMonitor::is_workspace_event("activewindow>>kitty,title"),
            "focus-only application changes must not refresh workspaces");
}

void test_modern_socket_path_resolution() {
    char pattern[] = "/tmp/realmheart-hypr-events-XXXXXX";
    const char* root = ::mkdtemp(pattern);
    if (root == nullptr) throw std::runtime_error("mkdtemp failed");

    const char* old_runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* old_signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const std::string runtime_backup = old_runtime ? old_runtime : "";
    const std::string signature_backup = old_signature ? old_signature : "";
    const bool had_runtime = old_runtime != nullptr;
    const bool had_signature = old_signature != nullptr;

    const std::string signature = "realmheart-test-instance";
    const auto socket_path = std::filesystem::path(root) / "hypr" / signature / ".socket2.sock";
    std::filesystem::create_directories(socket_path.parent_path());
    std::ofstream(socket_path) << "fixture";
    ::setenv("XDG_RUNTIME_DIR", root, 1);
    ::setenv("HYPRLAND_INSTANCE_SIGNATURE", signature.c_str(), 1);

    const auto resolved = realmheart::services::HyprlandEventMonitor::event_socket_path();
    require(resolved && *resolved == socket_path, "modern Hyprland event socket path must resolve");

    if (had_runtime) ::setenv("XDG_RUNTIME_DIR", runtime_backup.c_str(), 1);
    else ::unsetenv("XDG_RUNTIME_DIR");
    if (had_signature) ::setenv("HYPRLAND_INSTANCE_SIGNATURE", signature_backup.c_str(), 1);
    else ::unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
    try {
        test_workspace_event_filter();
        test_modern_socket_path_resolution();
    } catch (const std::exception& error) {
        std::cerr << "HyprlandEventMonitorTests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Hyprland event monitor tests passed\n";
    return 0;
}
