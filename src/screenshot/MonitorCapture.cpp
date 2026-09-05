#include "screenshot/MonitorCapture.hpp"

#include "core/Command.hpp"

#include <chrono>
#include <cstdlib>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace realmheart::screenshot {
namespace {

std::filesystem::path runtime_directory() {
    if (const char* xdg_runtime = std::getenv("XDG_RUNTIME_DIR");
        xdg_runtime != nullptr && *xdg_runtime != '\0') {
        return std::filesystem::path(xdg_runtime) / "realmheart";
    }
    return std::filesystem::temp_directory_path() /
        ("realmheart-" + std::to_string(static_cast<unsigned long>(geteuid())));
}

std::filesystem::path unique_capture_path() {
    const auto directory = runtime_directory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return {};

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return directory /
        ("screenshot-" + std::to_string(static_cast<unsigned long>(geteuid())) + "-" +
         std::to_string(static_cast<long long>(nonce)) + ".png");
}

CaptureResult failure(std::string message) {
    CaptureResult result;
    result.error = std::move(message);
    return result;
}

} // namespace

CaptureResult MonitorCapture::capture_once(const MonitorTarget& monitor) {
    if (monitor.connector.empty()) return failure("capture monitor has no connector name");
    if (!realmheart::core::command_exists("grim")) return failure("grim not found");

    const auto path = unique_capture_path();
    if (path.empty()) return failure("unable to create Realmheart runtime capture directory");

    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::seconds(5);
    options.max_output_bytes = 128 * 1024;

    const auto result = realmheart::core::run_capture(
        {"grim", "-o", monitor.connector, path.string()},
        options
    );
    if (!result.succeeded()) {
        remove_quietly(path);
        return failure(realmheart::core::command_failure_detail(
            result,
            "grim monitor capture failed"
        ));
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        remove_quietly(path);
        return failure("grim completed without producing a capture file");
    }

    CaptureResult capture;
    capture.path = path;
    return capture;
}

void MonitorCapture::remove_quietly(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace realmheart::screenshot
