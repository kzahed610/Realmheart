#include "services/NotesService.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace realmheart::services {
namespace {

std::atomic<std::uint64_t> temporary_sequence{0};
constexpr auto shutdown_grace = std::chrono::milliseconds{300};

enum class WriteResult {
    Committed,
    Stale,
    Failed,
    DurabilityUncertain,
};

enum class LoadResult {
    Missing,
    Loaded,
    Failed,
};

bool is_absolute_directory_root(const char* value) {
    return value != nullptr && *value != '\0' &&
        std::filesystem::path(value).is_absolute();
}

std::filesystem::path default_notes_path() {
    if (const char* config = std::getenv("XDG_CONFIG_HOME");
        is_absolute_directory_root(config)) {
        return std::filesystem::path(config) / "realmheart/notes.txt";
    }

    if (const char* home = std::getenv("HOME"); is_absolute_directory_root(home)) {
        return std::filesystem::path(home) / ".config/realmheart/notes.txt";
    }

    if (const passwd* account = ::getpwuid(::getuid());
        account != nullptr && is_absolute_directory_root(account->pw_dir)) {
        return std::filesystem::path(account->pw_dir) / ".config/realmheart/notes.txt";
    }

    std::error_code ec;
    auto temporary = std::filesystem::temp_directory_path(ec);
    if (ec || !temporary.is_absolute()) temporary = "/tmp";
    return temporary / ("realmheart-notes-" + std::to_string(::getuid()) + ".txt");
}

bool valid_note_text(const std::string& text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first == 0) return false;
        if (first <= 0x7f) {
            ++index;
            continue;
        }

        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            codepoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            codepoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            codepoint = first & 0x07;
        } else {
            return false;
        }

        if (index + length > text.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if ((length == 3 && codepoint < 0x800) ||
            (length == 4 && codepoint < 0x10000) ||
            codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        index += length;
    }
    return true;
}

LoadResult read_note_file(
    const std::filesystem::path& path,
    std::string& content
) {
    content.clear();
    if (!path.is_absolute() || path.filename().empty()) return LoadResult::Failed;

    const int fd = ::open(
        path.c_str(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW
    );
    if (fd < 0) return errno == ENOENT ? LoadResult::Missing : LoadResult::Failed;

    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) > NotesService::max_note_bytes) {
        ::close(fd);
        return LoadResult::Failed;
    }

    content.reserve(static_cast<std::size_t>(metadata.st_size));
    std::array<char, 16 * 1024> chunk{};
    while (true) {
        const ssize_t count = ::read(fd, chunk.data(), chunk.size());
        if (count > 0) {
            if (content.size() + static_cast<std::size_t>(count) > NotesService::max_note_bytes) {
                ::close(fd);
                content.clear();
                return LoadResult::Failed;
            }
            content.append(chunk.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        ::close(fd);
        content.clear();
        return LoadResult::Failed;
    }

    if (::close(fd) != 0 || !valid_note_text(content)) {
        content.clear();
        return LoadResult::Failed;
    }
    return LoadResult::Loaded;
}

void report_errno(const char* operation, int error) {
    std::cerr << "NotesService: " << operation << ": " << std::strerror(error) << '\n';
}

} // namespace

struct NotesService::State {
    explicit State(std::filesystem::path path, std::chrono::milliseconds delay)
        : notes_path(std::move(path)), debounce(delay) {}

    const std::filesystem::path notes_path;
    const std::chrono::milliseconds debounce;
    mutable std::mutex mutex;
    std::mutex commit_mutex;
    std::mutex io_mutex;
    std::condition_variable cv;
    std::condition_variable stopped_cv;
    std::string cached_content;
    SaveStateCallback save_state_callback;
    NotesSaveState save_state = NotesSaveState::Saved;
    std::uint64_t edit_generation = 0;
    bool dirty = false;
    bool retry_blocked = false;
    bool load_failed = false;
    bool load_failure_acknowledged = false;
    bool stopping = false;
    bool shutdown_attempted = false;
    bool worker_exited = false;
};

namespace {

WriteResult write_atomically(
    const std::shared_ptr<NotesService::State>& state,
    const std::string& content,
    std::uint64_t generation
) {
    if (content.size() > NotesService::max_note_bytes || !valid_note_text(content)) {
        return WriteResult::Failed;
    }

    const auto parent = state->notes_path.parent_path();
    const auto filename = state->notes_path.filename().string();
    if (!state->notes_path.is_absolute() || parent.empty() || filename.empty()) {
        return WriteResult::Failed;
    }

    std::lock_guard io_lock(state->io_mutex);
    const int directory_fd = ::open(
        parent.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (directory_fd < 0) {
        report_errno("open notes directory", errno);
        return WriteResult::Failed;
    }

    int temporary_fd = -1;
    std::string temporary_name;
    for (int attempt = 0; attempt < 32; ++attempt) {
        temporary_name = "." + filename + ".tmp." + std::to_string(::getpid()) + "." +
            std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed));
        temporary_fd = ::openat(
            directory_fd,
            temporary_name.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600
        );
        if (temporary_fd >= 0) break;
        if (errno != EEXIST) {
            report_errno("create temporary notes file", errno);
            ::close(directory_fd);
            return WriteResult::Failed;
        }
    }
    if (temporary_fd < 0) {
        ::close(directory_fd);
        return WriteResult::Failed;
    }

    const auto remove_temporary = [&] {
        ::unlinkat(directory_fd, temporary_name.c_str(), 0);
    };
    int write_error = 0;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t written = ::write(
            temporary_fd,
            content.data() + offset,
            content.size() - offset
        );
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        write_error = written == 0 ? EIO : errno;
        break;
    }
    if (write_error == 0 && ::fchmod(temporary_fd, 0600) != 0) write_error = errno;
    if (write_error == 0 && ::fsync(temporary_fd) != 0) write_error = errno;
    if (::close(temporary_fd) != 0 && write_error == 0) write_error = errno;
    temporary_fd = -1;
    if (write_error != 0) {
        report_errno("write temporary notes file", write_error);
        remove_temporary();
        ::close(directory_fd);
        return WriteResult::Failed;
    }

    {
        std::lock_guard commit_lock(state->commit_mutex);
        std::lock_guard state_lock(state->mutex);
        if (state->edit_generation != generation) {
            remove_temporary();
            ::close(directory_fd);
            return WriteResult::Stale;
        }
        if (::renameat(
                directory_fd,
                temporary_name.c_str(),
                directory_fd,
                filename.c_str()
            ) != 0) {
            const int error = errno;
            report_errno("replace notes file", error);
            remove_temporary();
            ::close(directory_fd);
            return WriteResult::Failed;
        }
    }

    int directory_error = 0;
    if (::fsync(directory_fd) != 0) directory_error = errno;
    if (::close(directory_fd) != 0 && directory_error == 0) directory_error = errno;
    if (directory_error != 0) {
        report_errno("sync notes directory", directory_error);
        return WriteResult::DurabilityUncertain;
    }
    return WriteResult::Committed;
}

NotesService::SaveStateCallback finalize_write(
    const std::shared_ptr<NotesService::State>& state,
    std::uint64_t generation,
    WriteResult result
) {
    std::lock_guard lock(state->mutex);
    const bool latest = state->edit_generation == generation;
    if (!latest) {
        state->save_state = NotesSaveState::Pending;
    } else {
        switch (result) {
        case WriteResult::Committed:
            state->dirty = false;
            state->retry_blocked = false;
            state->save_state = NotesSaveState::Saved;
            break;
        case WriteResult::DurabilityUncertain:
            state->dirty = false;
            state->retry_blocked = true;
            state->save_state = NotesSaveState::DurabilityUncertain;
            break;
        case WriteResult::Stale:
            state->save_state = NotesSaveState::Pending;
            break;
        case WriteResult::Failed:
            state->dirty = true;
            state->retry_blocked = true;
            state->save_state = NotesSaveState::Failed;
            break;
        }
    }
    state->cv.notify_all();
    return state->save_state_callback;
}

} // namespace

NotesService::NotesService()
    : NotesService(default_notes_path()) {}

NotesService::NotesService(
    std::filesystem::path notes_path,
    std::chrono::milliseconds debounce
) : state_(std::make_shared<State>(std::move(notes_path), std::max(debounce, std::chrono::milliseconds{1}))) {
    load_from_disk();
    worker_ = std::thread(&NotesService::worker_loop, state_);
}

NotesService::~NotesService() {
    const auto state = state_;
    if (state == nullptr) return;
    {
        std::lock_guard lock(state->mutex);
        state->save_state_callback = {};
        state->stopping = true;
    }
    state->cv.notify_all();

    if (worker_.joinable()) {
        std::unique_lock lock(state->mutex);
        // POSIX filesystem calls do not provide cancellation. Keep the shared
        // state alive if the worker exceeds this grace period so a pending
        // generation can still finish; process termination remains an explicit
        // durability boundary rather than a falsely claimed cancellation.
        const bool exited = state->stopped_cv.wait_for(
            lock,
            shutdown_grace,
            [&] { return state->worker_exited; }
        );
        lock.unlock();
        if (exited) worker_.join();
        else worker_.detach();
    }
}

void NotesService::load_from_disk() {
    const auto state = state_;
    std::string content;
    bool path_usable = state->notes_path.is_absolute() && !state->notes_path.filename().empty();
    if (path_usable) {
        const auto parent = state->notes_path.parent_path();
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) path_usable = false;
    }

    const LoadResult result = path_usable
        ? read_note_file(state->notes_path, content)
        : LoadResult::Failed;
    std::lock_guard lock(state->mutex);
    state->cached_content = std::move(content);
    state->load_failed = result == LoadResult::Failed;
    state->load_failure_acknowledged = false;
    state->save_state = state->load_failed
        ? NotesSaveState::LoadFailed
        : NotesSaveState::Saved;
}

std::string NotesService::get_content() const {
    std::lock_guard lock(state_->mutex);
    return state_->cached_content;
}

bool NotesService::set_content(const std::string& content) {
    const auto state = state_;
    if (content.size() > max_note_bytes || !valid_note_text(content)) {
        SaveStateCallback callback;
        {
            std::lock_guard lock(state->mutex);
            state->save_state = NotesSaveState::Rejected;
            callback = state->save_state_callback;
        }
        if (callback) callback(NotesSaveState::Rejected);
        return false;
    }

    SaveStateCallback callback;
    {
        std::lock_guard commit_lock(state->commit_mutex);
        std::lock_guard lock(state->mutex);
        if (state->stopping || (state->load_failed && !state->load_failure_acknowledged)) {
            return false;
        }
        state->cached_content = content;
        state->dirty = true;
        state->retry_blocked = false;
        ++state->edit_generation;
        state->save_state = NotesSaveState::Pending;
        callback = state->save_state_callback;
    }
    if (callback) callback(NotesSaveState::Pending);
    state->cv.notify_all();
    return true;
}

bool NotesService::acknowledge_load_failure() {
    std::lock_guard lock(state_->mutex);
    if (!state_->load_failed || state_->stopping) return false;
    state_->load_failure_acknowledged = true;
    return true;
}

bool NotesService::save() {
    const auto state = state_;
    std::string content;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping || (state->load_failed && !state->load_failure_acknowledged)) return false;
        content = state->cached_content;
        generation = state->edit_generation;
    }
    const WriteResult result = write_atomically(state, content, generation);
    const SaveStateCallback callback = finalize_write(state, generation, result);
    NotesSaveState current;
    {
        std::lock_guard lock(state->mutex);
        current = state->save_state;
    }
    if (callback) callback(current);
    return result == WriteResult::Committed;
}

void NotesService::set_save_state_callback(SaveStateCallback callback) {
    NotesSaveState current;
    {
        std::lock_guard lock(state_->mutex);
        state_->save_state_callback = std::move(callback);
        current = state_->save_state;
        callback = state_->save_state_callback;
    }
    if (callback) callback(current);
}

NotesSaveState NotesService::save_state() const {
    std::lock_guard lock(state_->mutex);
    return state_->save_state;
}

std::string NotesService::get_file_path() const {
    return state_->notes_path.string();
}

void NotesService::worker_loop(std::shared_ptr<State> state) {
    while (true) {
        std::string content;
        std::uint64_t generation = 0;
        {
            std::unique_lock lock(state->mutex);
            state->cv.wait(lock, [&] {
                return state->stopping || (state->dirty && !state->retry_blocked);
            });

            if (!state->stopping) {
                const auto observed_generation = state->edit_generation;
                const auto deadline = std::chrono::steady_clock::now() + state->debounce;
                if (state->cv.wait_until(lock, deadline, [&] {
                        return state->stopping || state->edit_generation != observed_generation;
                    })) {
                    continue;
                }
                if (!state->dirty) continue;
            }

            if (!state->dirty || (state->stopping && state->shutdown_attempted)) {
                state->worker_exited = true;
                state->stopped_cv.notify_all();
                return;
            }
            if (state->stopping) state->shutdown_attempted = true;
            content = state->cached_content;
            generation = state->edit_generation;
        }

        const WriteResult result = write_atomically(state, content, generation);
        const SaveStateCallback callback = finalize_write(state, generation, result);
        if (callback) {
            NotesSaveState current;
            {
                std::lock_guard lock(state->mutex);
                current = state->save_state;
            }
            callback(current);
        }
    }
}

} // namespace realmheart::services