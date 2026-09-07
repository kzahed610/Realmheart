#include "services/UtilityManager.hpp"
#include "services/ThemeService.hpp"
#include "services/MatugenParser.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <linux/memfd.h>
#include <limits>
#include <sstream>
#include <sys/syscall.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace realmheart::services {
namespace {

std::filesystem::path default_recorder_pid_path() {
    if (const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        runtime_dir != nullptr && *runtime_dir != '\0') {
        return std::filesystem::path(runtime_dir) / "realmheart/wf-recorder.pid";
    }
    return std::filesystem::temp_directory_path() /
        ("realmheart-" + std::to_string(static_cast<unsigned long>(geteuid())) + "-wf-recorder.pid");
}

bool ensure_parent_directory(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    if (parent.empty()) return true;
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    return !error;
}

bool read_recorder_pid(
    const std::filesystem::path& path,
    int& pid,
    std::string& expected_start_time
) {
    std::ifstream pid_file(path);
    long long parsed_pid = 0;
    if (!(pid_file >> parsed_pid >> expected_start_time) ||
        parsed_pid <= 0 || parsed_pid > std::numeric_limits<int>::max()) {
        return false;
    }
    pid = static_cast<int>(parsed_pid);
    return true;
}

std::string screenshot_helper_command() {
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !self.empty()) {
        const auto sibling = self.parent_path() / "realmheart-screenshot";
        if (::access(sibling.c_str(), X_OK) == 0) {
            return sibling.string();
        }
    }
    return "realmheart-screenshot";
}

struct MatugenSourceLease {
    int descriptor = -1;

    MatugenSourceLease() = default;

    ~MatugenSourceLease() {
        if (descriptor >= 0) ::close(descriptor);
    }

    MatugenSourceLease(const MatugenSourceLease&) = delete;
    MatugenSourceLease& operator=(const MatugenSourceLease&) = delete;
};

std::optional<std::string> matugen_input_path(
    const WallpaperSource& source,
    MatugenSourceLease& lease,
    std::string* error_message
) {
    constexpr std::size_t kMaxWallpaperSourceBytes = 128ULL * 1024ULL * 1024ULL;
    if (const auto path = source.external_path()) {
        std::error_code error;
        if (path->empty() || !std::filesystem::is_regular_file(*path, error) || error) {
            if (error_message != nullptr) *error_message = "wallpaper source path is invalid";
            return std::nullopt;
        }
        const auto file_size = std::filesystem::file_size(*path, error);
        if (error || file_size > kMaxWallpaperSourceBytes) {
            if (error_message != nullptr) *error_message = "wallpaper source exceeds the Matugen budget";
            return std::nullopt;
        }
        return path->string();
    }
    const std::string* bytes = source.bytes();
    if (bytes == nullptr || bytes->empty() || bytes->size() > kMaxWallpaperSourceBytes) {
        if (error_message != nullptr) *error_message = "wallpaper source exceeds the Matugen budget";
        return std::nullopt;
    }

    lease.descriptor = static_cast<int>(::syscall(
        SYS_memfd_create,
        "realmheart-wallpaper",
        MFD_ALLOW_SEALING
    ));
    if (lease.descriptor < 0) {
        if (error_message != nullptr) *error_message = "unable to create an owned Matugen input";
        return std::nullopt;
    }

    std::size_t written = 0;
    while (written < bytes->size()) {
        const ssize_t result = ::write(
            lease.descriptor,
            bytes->data() + written,
            bytes->size() - written
        );
        if (result > 0) {
            written += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            if (error_message != nullptr) *error_message = "unable to populate owned Matugen input";
            return std::nullopt;
        }
    }
    if (::fcntl(
            lease.descriptor,
            F_ADD_SEALS,
            F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL
        ) != 0) {
        if (error_message != nullptr) *error_message = "unable to seal owned Matugen input";
        return std::nullopt;
    }
    return "/proc/self/fd/" + std::to_string(lease.descriptor);
}

} // namespace


bool SystemUtilityExecutor::send_signal(int pid, int signal_number) {
    errno = 0;
    return ::kill(pid, signal_number) == 0;
}

UtilityManager::UtilityManager(
    std::shared_ptr<services::ThemeService> theme_service,
    std::unique_ptr<IUtilityExecutor> executor,
    std::filesystem::path recorder_pid_path,
    std::filesystem::path proc_root
) : executor_(std::move(executor)),
    recorder_pid_path_(recorder_pid_path.empty()
        ? default_recorder_pid_path()
        : std::move(recorder_pid_path)),
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

bool UtilityManager::launch_screenshot_tool() {
    return executor_->run_background({screenshot_helper_command()});
}

bool UtilityManager::set_wallpaper(const std::string& path) {
    return wallpaper_service_->set_wallpaper(path);
}

bool UtilityManager::choose_wallpaper() {
    return wallpaper_service_->choose_wallpaper();
}

std::optional<services::Palette> UtilityManager::generate_palette(
    const services::WallpaperSource& source,
    std::function<bool()> cancelled
) {
    MatugenSourceLease source_lease;
    std::string source_error;
    const auto image_path = matugen_input_path(source, source_lease, &source_error);
    if (!image_path) {
        std::cerr << "[Theme] Refusing to generate colors for an invalid wallpaper source: "
                  << source_error << '\n';
        return std::nullopt;
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
    options.cancelled = std::move(cancelled);

    const auto result = executor_->run_capture({
        "matugen",
        "image",
        *image_path,
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
        return std::nullopt;
    }
    if (result.output.empty()) {
        std::cerr << "[Theme] Matugen produced no JSON output\n";
        return std::nullopt;
    }
    if (result.truncated) {
        std::cerr << "[Theme] Matugen JSON output was truncated\n";
        return std::nullopt;
    }

    auto palette = MatugenParser::parse(result.output, ThemeMode::Dark);
    if (!palette) {
        std::cerr << "[Theme] Matugen output did not contain a usable dark palette\n";
        return std::nullopt;
    }

    return palette;
}

std::optional<services::Palette> UtilityManager::generate_palette(
    const std::string& path,
    std::function<bool()> cancelled
) {
    return generate_palette(services::WallpaperSource(path), std::move(cancelled));
}

bool UtilityManager::generate_colors(const std::string& path) {
    auto palette = generate_palette(path);
    if (!palette) return false;
    theme_service_->update_palette(std::move(*palette));
    return true;
}

std::optional<services::WallpaperSource> UtilityManager::load_wallpaper_source() {
    return wallpaper_service_->load_source();
}

std::string UtilityManager::load_wallpaper_path() {
    const auto source = load_wallpaper_source();
    return source ? source->string() : std::string{};
}

bool UtilityManager::start_recording(const std::string& path) {
    std::lock_guard lock(recorder_mutex_);
    if (!ensure_parent_directory(path) || !ensure_parent_directory(recorder_pid_path_)) {
        return false;
    }

    int existing_pid = 0;
    std::string existing_start_time;
    if (read_recorder_pid(recorder_pid_path_, existing_pid, existing_start_time) &&
        recorder_identity_matches(existing_pid, existing_start_time)) {
        return false;
    }
    {
        std::error_code error;
        std::filesystem::remove(recorder_pid_path_, error);
    }

    if (!executor_->run_background({
        "sh", "-c",
        "start=$(awk '{print $22}' /proc/$$/stat) || exit 1; "
        "printf '%s %s\\n' \"$$\" \"$start\" > \"$1\" || exit 1; "
        "exec wf-recorder -f \"$2\"",
        "realmheart-recorder",
        recorder_pid_path_.string(),
        path
    })) {
        return false;
    }

    // run_background only confirms that the wrapper reached exec. Wait for
    // the wrapper's PID file and for that PID to become wf-recorder before
    // claiming ownership. This closes the immediate start/stop race and makes
    // failed wf-recorder launches visible to the caller.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        int pid = 0;
        std::string start_time;
        if (read_recorder_pid(recorder_pid_path_, pid, start_time) &&
            recorder_identity_matches(pid, start_time)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::error_code error;
    std::filesystem::remove(recorder_pid_path_, error);
    return false;
}

bool UtilityManager::stop_recording() {
    std::lock_guard lock(recorder_mutex_);
    int pid = 0;
    std::string expected_start_time;
    if (!read_recorder_pid(recorder_pid_path_, pid, expected_start_time)) {
        std::error_code error;
        std::filesystem::remove(recorder_pid_path_, error);
        return false;
    }

    if (!recorder_identity_matches(pid, expected_start_time)) {
        std::error_code error;
        std::filesystem::remove(recorder_pid_path_, error);
        return false;
    }

    const bool signalled = executor_->send_signal(pid, SIGINT);
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
