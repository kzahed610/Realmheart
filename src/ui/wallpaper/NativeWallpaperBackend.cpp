#include "ui/wallpaper/NativeWallpaperBackend.hpp"
#include "wallpaper-native/NativeWallpaperContracts.hpp"

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

std::string base64_token(std::string_view value) {
    gchar* encoded = g_base64_encode(
        reinterpret_cast<const guchar*>(value.data()),
        value.size()
    );
    if (encoded == nullptr) return {};
    std::string result = encoded;
    g_free(encoded);
    return result;
}

std::string encoded_path_command(
    std::string_view verb,
    const std::filesystem::path& path,
    std::string* error_message
) {
    const std::string raw_path = path.string();
    gchar* encoded = g_base64_encode(
        reinterpret_cast<const guchar*>(raw_path.data()),
        raw_path.size()
    );
    if (encoded == nullptr) {
        set_error(error_message, "unable to encode wallpaper path");
        return {};
    }

    std::string command(verb);
    command.push_back(' ');
    command += encoded;
    command.push_back('\n');
    g_free(encoded);
    return command;
}

std::string encoded_output_path_command(
    std::string_view verb,
    std::string_view connector,
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (connector.empty()) {
        set_error(error_message, "native wallpaper output connector is empty");
        return {};
    }
    const std::string encoded_connector = base64_token(connector);
    const std::string encoded_path = base64_token(path.string());
    if (encoded_connector.empty() || encoded_path.empty()) {
        set_error(error_message, "unable to encode wallpaper output transaction");
        return {};
    }

    std::string command(verb);
    command.push_back(' ');
    command += encoded_connector;
    command.push_back(' ');
    command += encoded_path;
    command.push_back('\n');
    return command;
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
    {
        std::lock_guard lock(operation_mutex_);
        shutting_down_ = true;
    }
    stop();
}

bool NativeWallpaperBackend::initialize(std::string* error_message) {
    std::lock_guard lock(operation_mutex_);
    return initialize_locked(error_message);
}

bool NativeWallpaperBackend::initialize_locked(std::string* error_message) {
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
        stop_locked();
        return false;
    }
    g_object_ref(command_stream_);
    response_stream_ = g_data_input_stream_new(stdout_stream);

    if (!read_response("READY", error_message)) {
        stop_locked();
        return false;
    }

    initialized_ = true;
    struct ProcessWaitContext {
        std::weak_ptr<NativeWallpaperBackend> backend;
    };
    g_subprocess_wait_async(
        process_,
        nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer raw) {
            std::unique_ptr<ProcessWaitContext> context(
                static_cast<ProcessWaitContext*>(raw)
            );
            GError* error = nullptr;
            static_cast<void>(g_subprocess_wait_finish(
                G_SUBPROCESS(source), result, &error
            ));
            g_clear_error(&error);
            auto backend = context->backend.lock();
            if (!backend) return;

            std::lock_guard lock(backend->operation_mutex_);
            backend->handle_process_exit(G_SUBPROCESS(source));
        },
        new ProcessWaitContext{weak_from_this()}
    );
    return true;
}

void NativeWallpaperBackend::handle_process_exit(GSubprocess* source) noexcept {
    if (process_ != source) return;

    initialized_ = false;
    if (command_stream_ != nullptr) {
        g_output_stream_close(command_stream_, nullptr, nullptr);
        g_object_unref(command_stream_);
        command_stream_ = nullptr;
    }
    if (response_stream_ != nullptr) {
        g_object_unref(response_stream_);
        response_stream_ = nullptr;
    }
    process_ = nullptr;
    g_object_unref(source);

    const realmheart::wallpaper_native::NativeRecoveryState recovery_state{
        shutting_down_,
        last_replay_kind_ != ReplayKind::None,
        recovery_attempts_,
    };
    if (realmheart::wallpaper_native::native_restart_decision(recovery_state) !=
        realmheart::wallpaper_native::NativeRestartDecision::RestartAndReplay) {
        return;
    }

    ++recovery_attempts_;
    recovering_ = true;
    std::string error;
    const bool recovered = replay_last_committed_locked(&error);
    recovering_ = false;
    if (!recovered) {
        stop_locked();
    }
}

bool NativeWallpaperBackend::replay_last_committed_locked(
    std::string* error_message
) {
    if (last_replay_kind_ == ReplayKind::None ||
        (last_committed_owned_bytes_ == nullptr && last_committed_path_.empty())) {
        set_error(error_message, "native wallpaper renderer has no safe replay state");
        return false;
    }
    if (!initialize_locked(error_message)) return false;

    if (last_replay_kind_ == ReplayKind::Global) {
        if (last_committed_owned_bytes_ != nullptr) {
            return set_owned_wallpaper_locked(last_committed_owned_bytes_, error_message);
        }
        return set_wallpaper_locked(last_committed_path_, error_message);
    }

    WallpaperOutputTarget target;
    target.connector = last_committed_output_connector_;
    if (!target.valid()) {
        set_error(error_message, "native wallpaper replay target is unavailable");
        return false;
    }
    const bool prepared = last_committed_owned_bytes_ != nullptr
        ? prepare_owned_wallpaper_for_output_locked(
              last_committed_owned_bytes_, target, error_message
          )
        : prepare_wallpaper_for_output_locked(
              last_committed_path_, target, error_message
          );
    return prepared && commit_prepared_wallpaper_locked(error_message);
}

bool NativeWallpaperBackend::set_wallpaper(
    const WallpaperSource& source,
    std::string* error_message
) {
    std::lock_guard lock(operation_mutex_);
    if (source.is_owned()) {
        return set_owned_wallpaper_locked(source.owned_bytes_handle(), error_message);
    }
    const auto path = source.external_path();
    if (!path) {
        set_error(error_message, "native wallpaper source is empty or invalid");
        return false;
    }
    return set_wallpaper_locked(*path, error_message);
}

bool NativeWallpaperBackend::prepare_wallpaper(
    const WallpaperSource& source,
    std::string* error_message
) {
    std::lock_guard lock(operation_mutex_);
    if (source.is_owned()) {
        return prepare_owned_wallpaper_locked(source.owned_bytes_handle(), error_message);
    }
    const auto path = source.external_path();
    if (!path) {
        set_error(error_message, "native wallpaper source is empty or invalid");
        return false;
    }
    return prepare_wallpaper_locked(*path, error_message);
}

bool NativeWallpaperBackend::prepare_wallpaper_for_output(
    const WallpaperSource& source,
    const WallpaperOutputTarget& target,
    std::string* error_message
) {
    std::lock_guard lock(operation_mutex_);
    if (source.is_owned()) {
        return prepare_owned_wallpaper_for_output_locked(
            source.owned_bytes_handle(), target, error_message
        );
    }
    const auto path = source.external_path();
    if (!path) {
        set_error(error_message, "native wallpaper source is empty or invalid");
        return false;
    }
    return prepare_wallpaper_for_output_locked(*path, target, error_message);
}

bool NativeWallpaperBackend::commit_prepared_wallpaper(
    std::string* error_message
) {
    std::lock_guard lock(operation_mutex_);
    return commit_prepared_wallpaper_locked(error_message);
}

void NativeWallpaperBackend::discard_prepared_wallpaper() noexcept {
    std::lock_guard lock(operation_mutex_);
    const auto clear_state = [this] {
        prepared_replay_kind_ = ReplayKind::None;
        prepared_path_.clear();
        prepared_owned_bytes_.reset();
        prepared_output_connector_.clear();
    };
    if (!initialized_) {
        clear_state();
        return;
    }
    std::string ignored;
    if (!send_line("DISCARD\n", &ignored) || !read_response("OK", &ignored)) {
        stop_locked();
        clear_state();
        return;
    }
    clear_state();
}

bool NativeWallpaperBackend::set_wallpaper_locked(
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_ && !initialize_locked(error_message)) return false;

    const std::string command = encoded_path_command("SET", path, error_message);
    if (command.empty()) return false;

    if (!send_line(command, error_message) || !read_response("OK", error_message)) {
        stop_locked();
        return false;
    }
    last_replay_kind_ = ReplayKind::Global;
    last_committed_path_ = path;
    last_committed_owned_bytes_.reset();
    last_committed_output_connector_.clear();
    if (!recovering_) recovery_attempts_ = 0;
    return true;
}

bool NativeWallpaperBackend::set_owned_wallpaper_locked(
    const std::shared_ptr<const std::string>& bytes,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (bytes == nullptr || bytes->empty() ||
        bytes->size() > realmheart::wallpaper_native::kNativeMaxSourceBytes) {
        set_error(error_message, "owned wallpaper bytes exceed the decode budget or are empty");
        return false;
    }
    if (!initialized_ && !initialize_locked(error_message)) return false;
    if (!send_owned_bytes(
            realmheart::wallpaper_native::NativeBinaryCommand::Set,
            {}, *bytes, error_message
        ) || !read_response("OK", error_message)) {
        stop_locked();
        return false;
    }
    last_replay_kind_ = ReplayKind::Global;
    last_committed_path_.clear();
    last_committed_owned_bytes_ = bytes;
    last_committed_output_connector_.clear();
    if (!recovering_) recovery_attempts_ = 0;
    return true;
}

bool NativeWallpaperBackend::prepare_wallpaper_locked(
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_ && !initialize_locked(error_message)) return false;

    const std::string command = encoded_path_command("PREPARE", path, error_message);
    if (command.empty()) return false;
    if (!send_line(command, error_message) ||
        !read_response("PREPARED", error_message)) {
        stop_locked();
        return false;
    }
    prepared_replay_kind_ = ReplayKind::Global;
    prepared_path_ = path;
    prepared_owned_bytes_.reset();
    prepared_output_connector_.clear();
    return true;
}

bool NativeWallpaperBackend::prepare_owned_wallpaper_locked(
    const std::shared_ptr<const std::string>& bytes,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (bytes == nullptr || bytes->empty() ||
        bytes->size() > realmheart::wallpaper_native::kNativeMaxSourceBytes) {
        set_error(error_message, "owned wallpaper bytes exceed the decode budget or are empty");
        return false;
    }
    if (!initialized_ && !initialize_locked(error_message)) return false;
    if (!send_owned_bytes(
            realmheart::wallpaper_native::NativeBinaryCommand::Prepare,
            {}, *bytes, error_message
        ) || !read_response("PREPARED", error_message)) {
        stop_locked();
        return false;
    }
    prepared_replay_kind_ = ReplayKind::Global;
    prepared_path_.clear();
    prepared_owned_bytes_ = bytes;
    prepared_output_connector_.clear();
    return true;
}

bool NativeWallpaperBackend::prepare_wallpaper_for_output_locked(
    const std::filesystem::path& path,
    const WallpaperOutputTarget& target,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!target.valid() || target.connector.empty()) {
        set_error(
            error_message,
            "native per-output wallpaper apply requires a monitor connector"
        );
        return false;
    }
    if (!initialized_ && !initialize_locked(error_message)) return false;

    const std::string command = encoded_output_path_command(
        "PREPARE_OUTPUT", target.connector, path, error_message
    );
    if (command.empty()) return false;
    if (!send_line(command, error_message) ||
        !read_response("PREPARED", error_message)) {
        stop_locked();
        return false;
    }
    prepared_replay_kind_ = ReplayKind::Output;
    prepared_path_ = path;
    prepared_owned_bytes_.reset();
    prepared_output_connector_ = target.connector;
    return true;
}

bool NativeWallpaperBackend::prepare_owned_wallpaper_for_output_locked(
    const std::shared_ptr<const std::string>& bytes,
    const WallpaperOutputTarget& target,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!target.valid() || target.connector.empty()) {
        set_error(
            error_message,
            "native per-output wallpaper apply requires a monitor connector"
        );
        return false;
    }
    if (bytes == nullptr || bytes->empty() ||
        bytes->size() > realmheart::wallpaper_native::kNativeMaxSourceBytes) {
        set_error(error_message, "owned wallpaper bytes exceed the decode budget or are empty");
        return false;
    }
    if (!initialized_ && !initialize_locked(error_message)) return false;
    const std::string encoded_connector = base64_token(target.connector);
    if (encoded_connector.empty() || !send_owned_bytes(
            realmheart::wallpaper_native::NativeBinaryCommand::PrepareOutput,
            encoded_connector, *bytes, error_message
        ) || !read_response("PREPARED", error_message)) {
        stop_locked();
        return false;
    }
    prepared_replay_kind_ = ReplayKind::Output;
    prepared_path_.clear();
    prepared_owned_bytes_ = bytes;
    prepared_output_connector_ = target.connector;
    return true;
}

bool NativeWallpaperBackend::commit_prepared_wallpaper_locked(
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_ && !initialize_locked(error_message)) return false;

    // COMMIT keeps the old and prepared full-resolution textures resident in
    // the native renderer. No preview thumbnail is ever stretched fullscreen.
    if (!send_line("COMMIT\n", error_message) ||
        !read_response("OK", error_message)) {
        stop_locked();
        return false;
    }
    last_replay_kind_ = prepared_replay_kind_;
    last_committed_path_ = prepared_path_;
    last_committed_owned_bytes_ = std::move(prepared_owned_bytes_);
    last_committed_output_connector_ = prepared_output_connector_;
    prepared_replay_kind_ = ReplayKind::None;
    prepared_path_.clear();
    prepared_owned_bytes_.reset();
    prepared_output_connector_.clear();
    if (!recovering_) recovery_attempts_ = 0;
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

bool NativeWallpaperBackend::send_owned_bytes(
    realmheart::wallpaper_native::NativeBinaryCommand command,
    std::string_view encoded_output_token,
    const std::string& bytes,
    std::string* error_message
) {
    if (process_ == nullptr || command_stream_ == nullptr) {
        set_error(error_message, "native wallpaper renderer is not running");
        return false;
    }
    std::string header_error;
    const auto header = realmheart::wallpaper_native::encode_native_binary_header(
        command,
        encoded_output_token,
        bytes.size(),
        &header_error
    );
    if (!header) {
        set_error(error_message, header_error);
        return false;
    }

    GError* error = nullptr;
    GCancellable* cancellable = g_cancellable_new();
    gsize header_written = 0;
    gsize payload_written = 0;
    gboolean header_ok = FALSE;
    gboolean payload_ok = FALSE;
    gboolean flush_ok = FALSE;
    {
        CancellationDeadline deadline(
            cancellable,
            initialized_ ? kCommandTimeout : kStartupTimeout
        );
        header_ok = g_output_stream_write_all(
            command_stream_,
            header->data(),
            header->size(),
            &header_written,
            cancellable,
            &error
        );
        if (header_ok && header_written == header->size()) {
            payload_ok = g_output_stream_write_all(
                command_stream_,
                bytes.data(),
                bytes.size(),
                &payload_written,
                cancellable,
                &error
            );
        }
        if (payload_ok && payload_written == bytes.size()) {
            flush_ok = g_output_stream_flush(command_stream_, cancellable, &error);
        }
    }

    if (!header_ok || header_written != header->size() ||
        !payload_ok || payload_written != bytes.size() || !flush_ok) {
        set_error(
            error_message,
            error != nullptr ? error->message
                             : "unable to send owned wallpaper bytes to native renderer"
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
    std::lock_guard lock(operation_mutex_);
    stop_locked();
}

void NativeWallpaperBackend::stop_locked() noexcept {
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
    prepared_replay_kind_ = ReplayKind::None;
    prepared_path_.clear();
    prepared_owned_bytes_.reset();
    prepared_output_connector_.clear();
}

} // namespace realmheart::ui::wallpaper
