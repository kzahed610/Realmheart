#include "ui/wallpaper/NativeWallpaperBackend.hpp"

#include <glib.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <string>
#include <string_view>

namespace realmheart::ui::wallpaper {

namespace {

void set_error(std::string* destination, const std::string& message) {
    if (destination != nullptr) *destination = message;
}

std::string decode_error(std::string_view encoded) {
    gsize decoded_size = 0;
    guchar* decoded = g_base64_decode(std::string(encoded).c_str(), &decoded_size);
    if (decoded == nullptr) return "native wallpaper renderer reported an error";
    std::string result(
        reinterpret_cast<const char*>(decoded),
        static_cast<std::size_t>(decoded_size)
    );
    g_free(decoded);
    return result;
}

class CancellationDeadline {
public:
    CancellationDeadline(GCancellable* cancellable, std::chrono::milliseconds timeout)
        : cancellable_(cancellable), worker_([this, timeout] {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, timeout, [this] { return completed_; })) {
                g_cancellable_cancel(cancellable_);
            }
        }) {}

    ~CancellationDeadline() {
        {
            std::lock_guard lock(mutex_);
            completed_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

private:
    GCancellable* cancellable_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool completed_ = false;
    std::thread worker_;
};

constexpr auto kStartupTimeout = std::chrono::seconds(2);
constexpr auto kCommandTimeout = std::chrono::seconds(5);
constexpr auto kShutdownTimeout = std::chrono::milliseconds(250);

} // namespace

NativeWallpaperBackend::~NativeWallpaperBackend() {
    stop();
}

bool NativeWallpaperBackend::initialize(std::string* error_message) {
    if (error_message != nullptr) error_message->clear();
    if (initialized_) return true;

    const std::string executable = find_renderer_executable();
    if (executable.empty()) {
        set_error(
            error_message,
            "realmheart-wallpaper-renderer was not found; build the optional native renderer or set REALMHEART_WALLPAPER_RENDERER"
        );
        return false;
    }

    GError* error = nullptr;
    process_ = g_subprocess_new(
        static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDIN_PIPE |
            G_SUBPROCESS_FLAGS_STDOUT_PIPE
        ),
        &error,
        executable.c_str(),
        "--stdio",
        nullptr
    );

    if (process_ == nullptr) {
        set_error(
            error_message,
            error != nullptr ? error->message : "unable to launch native wallpaper renderer"
        );
        if (error != nullptr) g_error_free(error);
        return false;
    }
    if (error != nullptr) g_error_free(error);

    command_stream_ = g_subprocess_get_stdin_pipe(process_);
    GInputStream* stdout_stream = g_subprocess_get_stdout_pipe(process_);
    if (command_stream_ == nullptr || stdout_stream == nullptr) {
        set_error(error_message, "native wallpaper renderer did not expose IPC streams");
        stop();
        return false;
    }
    g_object_ref(command_stream_);
    response_stream_ = g_data_input_stream_new(stdout_stream);

    if (!read_response("READY", error_message)) {
        stop();
        return false;
    }

    initialized_ = true;
    return true;
}

bool NativeWallpaperBackend::set_wallpaper(
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_ && !initialize(error_message)) return false;

    const std::string raw_path = path.string();
    gchar* encoded = g_base64_encode(
        reinterpret_cast<const guchar*>(raw_path.data()),
        raw_path.size()
    );
    if (encoded == nullptr) {
        set_error(error_message, "unable to encode wallpaper path");
        return false;
    }

    std::string command = "SET ";
    command += encoded;
    command.push_back('\n');
    g_free(encoded);

    if (!send_line(command, error_message) || !read_response("OK", error_message)) {
        stop();
        return false;
    }
    return true;
}

std::string NativeWallpaperBackend::find_renderer_executable() const {
    if (const char* configured = std::getenv("REALMHEART_WALLPAPER_RENDERER");
        configured != nullptr && *configured != '\0') {
        return configured;
    }

    GError* link_error = nullptr;
    gchar* self_path = g_file_read_link("/proc/self/exe", &link_error);
    if (self_path != nullptr) {
        gchar* directory = g_path_get_dirname(self_path);
        gchar* sibling = g_build_filename(
            directory,
            "realmheart-wallpaper-renderer",
            nullptr
        );
        const bool executable = g_file_test(sibling, G_FILE_TEST_IS_EXECUTABLE);
        std::string result = executable ? sibling : "";
        g_free(sibling);
        g_free(directory);
        g_free(self_path);
        if (!result.empty()) return result;
    } else if (link_error != nullptr) {
        g_error_free(link_error);
    }

    gchar* found = g_find_program_in_path("realmheart-wallpaper-renderer");
    if (found == nullptr) return {};

    std::string path = found;
    g_free(found);
    return path;
}

bool NativeWallpaperBackend::send_line(
    const std::string& line,
    std::string* error_message
) {
    if (process_ == nullptr || command_stream_ == nullptr) {
        set_error(error_message, "native wallpaper renderer is not running");
        return false;
    }

    gsize written = 0;
    GError* error = nullptr;
    GCancellable* cancellable = g_cancellable_new();
    gboolean write_ok = FALSE;
    gboolean flush_ok = FALSE;
    {
        CancellationDeadline deadline(
            cancellable,
            initialized_ ? kCommandTimeout : kStartupTimeout
        );
        write_ok = g_output_stream_write_all(
            command_stream_, line.data(), line.size(), &written, cancellable, &error
        );
        if (write_ok && written == line.size()) {
            flush_ok = g_output_stream_flush(command_stream_, cancellable, &error);
        }
    }

    if (!write_ok || written != line.size() || !flush_ok) {
        set_error(
            error_message,
            error != nullptr ? error->message : "unable to send command to native wallpaper renderer"
        );
        g_clear_error(&error);
        g_object_unref(cancellable);
        return false;
    }

    g_clear_error(&error);
    g_object_unref(cancellable);
    return true;
}

bool NativeWallpaperBackend::read_response(
    const char* expected_success,
    std::string* error_message
) {
    if (response_stream_ == nullptr) {
        set_error(error_message, "native wallpaper renderer response stream is unavailable");
        return false;
    }

    gsize length = 0;
    GError* error = nullptr;
    GCancellable* cancellable = g_cancellable_new();
    gchar* line = nullptr;
    {
        CancellationDeadline deadline(
            cancellable,
            initialized_ ? kCommandTimeout : kStartupTimeout
        );
        line = g_data_input_stream_read_line(
            response_stream_, &length, cancellable, &error
        );
    }
    g_object_unref(cancellable);

    if (line == nullptr) {
        set_error(
            error_message,
            error != nullptr ? error->message : "native wallpaper renderer closed its response stream"
        );
        g_clear_error(&error);
        return false;
    }
    g_clear_error(&error);

    const std::string response(line, length);
    g_free(line);
    if (response == expected_success) return true;

    constexpr std::string_view error_prefix = "ERROR ";
    if (response.starts_with(error_prefix)) {
        set_error(error_message, decode_error(response.substr(error_prefix.size())));
    } else {
        set_error(error_message, "unexpected native wallpaper renderer response: " + response);
    }
    return false;
}

void NativeWallpaperBackend::stop() noexcept {
    // Closing stdin is a graceful protocol shutdown: the renderer treats EOF
    // exactly like QUIT. Avoid a final synchronous write that could otherwise
    // spend the normal command deadline waiting on an already-wedged helper.
    if (command_stream_ != nullptr) {
        g_output_stream_close(command_stream_, nullptr, nullptr);
        g_object_unref(command_stream_);
        command_stream_ = nullptr;
    }
    if (response_stream_ != nullptr) {
        g_object_unref(response_stream_);
        response_stream_ = nullptr;
    }

    if (process_ != nullptr) {
        GCancellable* cancellable = g_cancellable_new();
        {
            CancellationDeadline deadline(cancellable, kShutdownTimeout);
            GError* error = nullptr;
            if (!g_subprocess_wait(process_, cancellable, &error)) {
                g_clear_error(&error);
                g_subprocess_force_exit(process_);
            }
        }
        g_object_unref(cancellable);
        g_object_unref(process_);
        process_ = nullptr;
    }

    initialized_ = false;
}

} // namespace realmheart::ui::wallpaper
