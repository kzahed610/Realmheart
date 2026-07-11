#include "services/UtilityManager.hpp"
#include <iostream>
#include <sstream>
#include <filesystem>

namespace realmheart::services {

UtilityManager::UtilityManager(std::unique_ptr<IUtilityExecutor> executor) 
    : executor_(std::move(executor)) {}

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
    return executor_->run_background({"/home/zahed/.config/realmheart/scripts/colors/switchwall.sh", "--image", path});
}

bool UtilityManager::choose_wallpaper() {
    return executor_->run_background({"/home/zahed/.config/realmheart/scripts/colors/switchwall.sh"});
}

bool UtilityManager::generate_colors(const std::string& /*path*/) {
    return executor_->run_background({"/home/zahed/.config/realmheart/scripts/colors/switchwall.sh", "--noswitch"});
}

bool UtilityManager::start_recording(const std::string& path) {
    if (const auto parent = std::filesystem::path(path).parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    return executor_->run_background({"wf-recorder", "-f", path});
}

bool UtilityManager::stop_recording() {
    // wf-recorder stops when it receives a SIGINT. 
    // Since we run it via run_background, we need to pkill it.
    return executor_->run_background({"pkill", "-INT", "wf-recorder"});
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
