#include "services/UtilityManager.hpp"
#include "services/ThemeService.hpp"
#include "services/MatugenParser.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <unistd.h>

namespace realmheart::services {

UtilityManager::UtilityManager(
    std::shared_ptr<services::ThemeService> theme_service,
    std::unique_ptr<IUtilityExecutor> executor,
    std::filesystem::path recorder_pid_path,
    std::filesystem::path proc_root
) : executor_(std::move(executor)),
    recorder_pid_path_(std::move(recorder_pid_path)),
    proc_root_(std::move(proc_root)),
    theme_service_(std::move(theme_service)) {}

UtilityManager::UtilityManager(
    std::unique_ptr<IUtilityExecutor> executor,
    std::filesystem::path recorder_pid_path,
    std::filesystem::path proc_root
) : UtilityManager(
        std::make_shared<services::ThemeService>(),
        std::move(executor),
        std::move(recorder_pid_path),
        std::move(proc_root)
    ) {}

UtilityManager::~UtilityManager() = default;

bool UtilityManager::take_screenshot_full(const std::string& path) {
    if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    return executor_->run_background({"grim", path});
}

bool UtilityManager::take_screenshot_area(const std::string& path) {
    if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    return executor_->run_background({
        "sh", "-c", "geometry=$(slurp) || exit $?; grim -g \"$geometry\" \"$1\"", "realmheart-screenshot", path
    });
}

bool UtilityManager::take_screenshot_area_to_clipboard() {
    return executor_->run_background({
        "sh", "-c", "geometry=$(slurp) || exit $?; if [ -n \"$geometry\" ]; then grim -g \"$geometry\" - | wl-copy --type image/png; fi",
        "realmheart-screenshot-area"
    });
}

bool UtilityManager::extract_text_from_area() {
    return executor_->run_background({
        "sh", "-c",
        "image=$(mktemp /tmp/realmheart-ocr.XXXXXX.png) || exit 1; "
        "trap 'rm -f \"$image\"' EXIT; "
        "geometry=$(slurp) || exit $?; "
        "grim -g \"$geometry\" \"$image\" && tesseract \"$image\" stdout -l eng 2>/dev/null | wl-copy"
    });
}

bool UtilityManager::set_wallpaper(const std::string& path) {
    return wallpaper_service_->set_wallpaper(path);
}

bool UtilityManager::choose_wallpaper() {
    return wallpaper_service_->choose_wallpaper();
}

bool UtilityManager::generate_colors(const std::string& path) {
    const std::filesystem::path image_path(path);
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(image_path, error) || error) {
        std::cerr << "[Theme] Refusing to generate colors for an invalid wallpaper path: "
                  << path << '\n';
        return false;
    }

    // Matugen 4.x changed the default JSON layout. --old-json-output gives us a
    // stable machine-readable shape, while --source-color-index avoids the new
    // interactive source-color prompt. Matugen requires --quiet when --json is
    // used; without it, status/table output is mixed into stdout and JSON parsing
    // fails. Invoke Matugen directly so paths remain argv-safe and failures remain
    // visible in the captured diagnostic output.
    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::seconds(10);
    options.max_output_bytes = 512 * 1024;

    const auto result = executor_->run_capture({
        "matugen",
        "image",
        image_path.string(),
        "--dry-run",
        "--json",
        "hex",
        "--old-json-output",
        "--source-color-index",
        "0",
        "--quiet"
    }, options);

    if (!result.succeeded()) {
        std::cerr << "[Theme] Matugen failed: "
                  << realmheart::core::command_failure_detail(
                         result,
                         "matugen color generation failed"
                     )
                  << '\n';
        return false;
    }
    if (result.output.empty()) {
        std::cerr << "[Theme] Matugen produced no JSON output\n";
        return false;
    }
    if (result.truncated) {
        std::cerr << "[Theme] Matugen JSON output was truncated\n";
        return false;
    }

    auto palette = MatugenParser::parse(result.output, ThemeMode::Dark);
    if (!palette) {
        std::cerr << "[Theme] Matugen output did not contain a usable dark palette\n";
        return false;
    }

    theme_service_->update_palette(std::move(*palette));
    return true;
}

std::string UtilityManager::load_wallpaper_path() {
    auto path = wallpaper_service_->load_path();
    return path ? path->string() : "";
}

bool UtilityManager::start_recording(const std::string& path) {
    if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    if (const auto parent = recorder_pid_path_.parent_path(); !parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }

    return executor_->run_background({
        "sh", "-c",
        "start=$(awk '{print $22}' /proc/$$/stat) || exit 1; "
        "printf '%s %s\\n' \"$$\" \"$start\" > \"$1\" || exit 1; "
        "exec wf-recorder -f \"$2\"",
        "realmheart-recorder",
        recorder_pid_path_.string(),
        path
    });
}

bool UtilityManager::stop_recording() {
    std::ifstream pid_file(recorder_pid_path_);
    long long parsed_pid = 0;
    std::string expected_start_time;
    if (!(pid_file >> parsed_pid >> expected_start_time) ||
        parsed_pid <= 0 || parsed_pid > std::numeric_limits<int>::max()) {
        std::error_code error;
        std::filesystem::remove(recorder_pid_path_, error);
        return false;
    }

    const int pid = static_cast<int>(parsed_pid);
    if (!recorder_identity_matches(pid, expected_start_time)) {
        std::error_code error;
        std::filesystem::remove(recorder_pid_path_, error);
        return false;
    }

    const bool signalled = executor_->run_background({"kill", "-INT", std::to_string(pid)});
    if (signalled) {
        std::error_code error;
        std::filesystem::remove(recorder_pid_path_, error);
    }
    return signalled;
}

bool UtilityManager::recorder_identity_matches(int pid, const std::string& expected_start_time) const {
    const auto process_dir = proc_root_ / std::to_string(pid);

    std::ifstream comm_file(process_dir / "comm");
    std::string comm;
    std::getline(comm_file, comm);
    if (comm != "wf-recorder") return false;

    std::ifstream stat_file(process_dir / "stat");
    std::string stat;
    std::getline(stat_file, stat);
    const auto command_end = stat.rfind(')');
    if (command_end == std::string::npos || command_end + 2 >= stat.size()) return false;

    std::istringstream fields(stat.substr(command_end + 2));
    std::string value;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> value)) return false;
    }
    return value == expected_start_time;
}

bool UtilityManager::copy_to_clipboard(const std::string& text) {
    return executor_->run_background({"sh", "-c", "printf %s \"$1\" | wl-copy", "realmheart-clipboard", text});
}

std::string UtilityManager::paste_from_clipboard() {
    auto result = executor_->run_capture({"wl-paste"});
    return result.output;
}

bool UtilityManager::launch_wofi() {
    return executor_->run_background({"wofi", "--show", "drun"});
}

} // namespace realmheart::services
