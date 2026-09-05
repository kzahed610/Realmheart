#include "screenshot/MonitorResolver.hpp"
#include "screenshot/ScreenshotOverlay.hpp"
#include "screenshot/WaylandScreencopy.hpp"

#include <gio/gio.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

class ScreenshotSessionLock {
public:
    ScreenshotSessionLock() {
        const char* runtime_dir = g_get_user_runtime_dir();
        if (runtime_dir == nullptr || *runtime_dir == '\0') return;

        const std::string path = std::string{runtime_dir} +
            "/realmheart-screenshot.lock";
        fd_ = ::open(path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
        if (fd_ < 0) return;

        if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
            acquired_ = true;
            return;
        }

        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            duplicate_ = true;
        }
        ::close(fd_);
        fd_ = -1;
    }

    ScreenshotSessionLock(const ScreenshotSessionLock&) = delete;
    ScreenshotSessionLock& operator=(const ScreenshotSessionLock&) = delete;

    ~ScreenshotSessionLock() {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }

    [[nodiscard]] bool duplicate() const { return duplicate_; }
    [[nodiscard]] bool acquired() const { return acquired_; }

private:
    int fd_ = -1;
    bool acquired_ = false;
    bool duplicate_ = false;
};

void report_fatal_failure(const std::string& detail) {
    std::cerr << "[Screenshot] " << detail << '\n';

    gchar* notify_path = g_find_program_in_path("notify-send");
    if (notify_path == nullptr) return;

    GError* spawn_error = nullptr;
    GSubprocess* process = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE,
        &spawn_error,
        notify_path,
        "--app-name=Realmheart",
        "Realmheart Screenshot",
        detail.c_str(),
        nullptr
    );
    g_free(notify_path);
    if (process != nullptr) g_object_unref(process);
    if (spawn_error != nullptr) g_error_free(spawn_error);
}

} // namespace

int main() {
    using namespace realmheart::screenshot;
    using Clock = std::chrono::steady_clock;

    ScreenshotSessionLock session_lock;
    if (session_lock.duplicate()) {
        if (std::getenv("REALMHEART_SCREENSHOT_DEBUG") != nullptr) {
            std::cerr << "[Screenshot] Screenshot overlay is already active; "
                         "ignoring duplicate invocation\n";
        }
        return 0;
    }
    if (!session_lock.acquired() &&
        std::getenv("REALMHEART_SCREENSHOT_DEBUG") != nullptr) {
        std::cerr << "[Screenshot] Runtime lock unavailable; continuing without "
                     "single-instance protection\n";
    }

    const auto process_start = Clock::now();
    const bool timing = std::getenv("REALMHEART_SCREENSHOT_TIMING") != nullptr;

    const auto resolve_started = Clock::now();
    const auto resolved = MonitorResolver::detect_under_cursor();
    const auto resolve_done = Clock::now();
    if (!resolved.monitor) {
        report_fatal_failure("Unable to resolve screenshot monitor · " + resolved.error);
        return 1;
    }

    const auto capture_started = Clock::now();
    const auto captured = WaylandScreencopy::capture_output(
        resolved.monitor->connector
    );
    const auto capture_done = Clock::now();
    if (!captured.ok) {
        report_fatal_failure("Unable to capture frozen frame · " + captured.error);
        return 1;
    }

    if (timing) {
        const auto ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        std::cerr << "[Screenshot timing] monitor resolve "
                  << ms(resolve_done - resolve_started) << " ms\n";
        std::cerr << "[Screenshot timing] screencopy "
                  << ms(capture_done - capture_started) << " ms\n";
        std::cerr << "[Screenshot timing] ready for overlay "
                  << ms(capture_done - process_start) << " ms\n";
    }

    // Critical-path rule: once the exact frame exists, show it immediately.
    // Semantic + OpenCV target discovery happens only after the layer maps.
    return ScreenshotOverlay::run(
        captured.frame,
        *resolved.monitor,
        process_start
    );
}
