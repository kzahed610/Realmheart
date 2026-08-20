#include "services/ProphecyCaptureService.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <system_error>

namespace realmheart::services {

namespace {

std::filesystem::path prophecy_dir() {
    const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!xdg_runtime) return {};
    return std::filesystem::path(xdg_runtime) / "realmheart" / "prophecy";
}

// Get the active workspace ID from hyprctl.
int get_active_workspace_id() {
    FILE* pipe = popen("hyprctl activeworkspace -j 2>/dev/null", "r");
    if (!pipe) return -1;

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int ret = pclose(pipe);
    if (ret != 0) return -1;

    // Parse JSON: "id": <number>
    std::regex re(R"("id"\s*:\s*(\d+))");
    std::smatch match;
    if (std::regex_search(output, match, re)) {
        return std::stoi(match[1].str());
    }
    return -1;
}

} // namespace

std::filesystem::path ProphecyCaptureService::ensure_prophecy_dir() {
    auto dir = prophecy_dir();
    if (dir.empty()) return {};

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[prophecy] failed to create dir: " << ec.message() << "\n";
        return {};
    }
    // Owner-only access (0700).
    std::filesystem::permissions(dir,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, ec);

    return dir;
}

std::filesystem::path ProphecyCaptureService::capture_active_workspace() {
    int ws_id = get_active_workspace_id();
    if (ws_id <= 0) {
        std::cerr << "[prophecy] could not determine active workspace\n";
        return {};
    }
    return capture_workspace(ws_id);
}

std::filesystem::path ProphecyCaptureService::capture_workspace(int workspace_id) {
    if (workspace_id <= 0) return {};

    auto dir = ensure_prophecy_dir();
    if (dir.empty()) return {};

    std::string filename = "ws-" + std::to_string(workspace_id) + ".png";
    std::filesystem::path path = dir / filename;

    // Capture with grim. Suppress stderr to avoid noise.
    std::string cmd = "grim " + path.string() + " 2>/dev/null";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        std::cerr << "[prophecy] grim failed for ws " << workspace_id
                  << " (exit=" << ret << ")\n";
        return {};
    }

    // Verify.
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) ||
        std::filesystem::file_size(path, ec) == 0) {
        std::cerr << "[prophecy] screenshot missing or empty: " << path << "\n";
        return {};
    }

    std::cerr << "[prophecy] captured ws " << workspace_id << ": " << path
              << " (" << std::filesystem::file_size(path) << " bytes)\n";
    return path;
}

std::vector<ProphecyCaptureService::WorkspaceScreenshot>
ProphecyCaptureService::list_screenshots() {
    static constexpr int kMaxScreenshots = 5;

    std::vector<WorkspaceScreenshot> result;
    auto dir = prophecy_dir();
    if (dir.empty()) return result;

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return result;

    // Match ws-<id>.png files.
    std::regex re(R"(ws-(\d+)\.png)");
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        std::smatch match;
        if (std::regex_match(name, match, re)) {
            result.push_back({
                .workspace_id = std::stoi(match[1].str()),
                .path = entry.path()
            });
        }
    }

    // Sort by workspace ID for consistent ordering.
    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) { return a.workspace_id < b.workspace_id; });

    // If more than kMaxScreenshots, remove the oldest (lowest ID = oldest).
    while (static_cast<int>(result.size()) > kMaxScreenshots) {
        std::filesystem::remove(result.front().path, ec);
        std::cerr << "[prophecy] removed old screenshot: ws-"
                  << result.front().workspace_id << ".png\n";
        result.erase(result.begin());
    }

    return result;
}

void ProphecyCaptureService::cleanup() {
    auto dir = prophecy_dir();
    if (dir.empty()) return;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file()) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

} // namespace realmheart::services
