#include "services/NotesService.hpp"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace realmheart::services {
namespace {

std::filesystem::path default_notes_path() {
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && *config != '\0') {
        return std::filesystem::path(config) / "realmheart/notes.txt";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config/realmheart/notes.txt";
    }
    return std::filesystem::temp_directory_path() / "realmheart-notes.txt";
}

} // namespace

NotesService::NotesService()
    : NotesService(default_notes_path()) {}

NotesService::NotesService(
    std::filesystem::path notes_path,
    std::chrono::milliseconds debounce
) : notes_path_(std::move(notes_path)), debounce_(debounce) {
    if (const auto parent = notes_path_.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "NotesService: failed to create notes directory: " << ec.message() << '\n';
        }
    }

    load_from_disk();
    worker_ = std::thread(&NotesService::worker_loop, this);
}

NotesService::~NotesService() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void NotesService::load_from_disk() {
    std::ifstream file(notes_path_, std::ios::binary);
    std::string content;
    if (file.is_open()) {
        std::ostringstream buffer;
        buffer << file.rdbuf();
        content = buffer.str();
    }

    std::lock_guard lock(mutex_);
    cached_content_ = std::move(content);
}

std::string NotesService::get_content() {
    std::lock_guard lock(mutex_);
    return cached_content_;
}

void NotesService::set_content(const std::string& content) {
    SaveStateCallback callback;
    {
        std::lock_guard lock(mutex_);
        cached_content_ = content;
        dirty_ = true;
        ++edit_generation_;
        save_state_ = NotesSaveState::Pending;
        callback = save_state_callback_;
    }
    if (callback) callback(NotesSaveState::Pending);
    cv_.notify_one();
}

bool NotesService::save() {
    std::string content;
    std::size_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        content = cached_content_;
        generation = edit_generation_;
    }

    const bool written = write_atomically(content);
    NotesSaveState state = NotesSaveState::Pending;
    SaveStateCallback callback;
    {
        std::lock_guard lock(mutex_);
        const bool latest = edit_generation_ == generation;
        if (written && latest) dirty_ = false;
        state = !latest
            ? NotesSaveState::Pending
            : (written ? NotesSaveState::Saved : NotesSaveState::Failed);
        save_state_ = state;
        callback = save_state_callback_;
    }
    if (callback) callback(state);
    return written;
}

void NotesService::set_save_state_callback(SaveStateCallback callback) {
    NotesSaveState current = NotesSaveState::Saved;
    {
        std::lock_guard lock(mutex_);
        save_state_callback_ = std::move(callback);
        current = save_state_;
        callback = save_state_callback_;
    }
    if (callback) callback(current);
}

NotesSaveState NotesService::save_state() const {
    std::lock_guard lock(mutex_);
    return save_state_;
}

void NotesService::worker_loop() {
    std::unique_lock lock(mutex_);

    while (true) {
        cv_.wait(lock, [this] { return stopping_ || dirty_; });

        if (!stopping_) {
            const auto observed_generation = edit_generation_;
            const auto deadline = std::chrono::steady_clock::now() + debounce_;
            if (cv_.wait_until(lock, deadline, [this, observed_generation] {
                    return stopping_ || edit_generation_ != observed_generation;
                }) && !stopping_) {
                continue;
            }
        }

        if (!dirty_ && stopping_) return;
        const std::string content = cached_content_;
        const std::size_t generation = edit_generation_;
        lock.unlock();
        const bool written = write_atomically(content);
        lock.lock();

        const bool latest = edit_generation_ == generation;
        if (written && latest) dirty_ = false;
        const NotesSaveState state = !latest
            ? NotesSaveState::Pending
            : (written ? NotesSaveState::Saved : NotesSaveState::Failed);
        save_state_ = state;
        const SaveStateCallback callback = save_state_callback_;
        const bool stopping = stopping_;
        lock.unlock();
        if (callback) callback(state);
        if (stopping) return;
        lock.lock();
    }
}

bool NotesService::write_atomically(const std::string& content) {
    // save() and the debounce worker may overlap. Serialize only disk I/O—not
    // access to the in-memory note—so both paths can safely share atomic replace.
    std::lock_guard io_lock(io_mutex_);
    const auto temporary_path = std::filesystem::path(notes_path_.string() + ".tmp");

    const int temporary_fd = ::open(
        temporary_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0600
    );
    if (temporary_fd < 0) {
        std::cerr << "NotesService: failed to open temporary notes file: "
                  << std::strerror(errno) << '\n';
        return false;
    }

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

    if (write_error == 0 && ::fsync(temporary_fd) != 0) write_error = errno;
    if (::close(temporary_fd) != 0 && write_error == 0) write_error = errno;
    if (write_error != 0) {
        std::cerr << "NotesService: failed to durably write temporary notes file: "
                  << std::strerror(write_error) << '\n';
        std::error_code remove_error;
        std::filesystem::remove(temporary_path, remove_error);
        return false;
    }

    if (::rename(temporary_path.c_str(), notes_path_.c_str()) != 0) {
        std::cerr << "NotesService: failed to replace notes file: "
                  << std::strerror(errno) << '\n';
        std::error_code remove_error;
        std::filesystem::remove(temporary_path, remove_error);
        return false;
    }

    const auto parent = notes_path_.parent_path().empty()
        ? std::filesystem::path{"."}
        : notes_path_.parent_path();
    const int directory_fd = ::open(
        parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC
    );
    if (directory_fd < 0) {
        std::cerr << "NotesService: failed to open notes directory for fsync: "
                  << std::strerror(errno) << '\n';
        return false;
    }

    int directory_error = 0;
    if (::fsync(directory_fd) != 0) directory_error = errno;
    if (::close(directory_fd) != 0 && directory_error == 0) {
        directory_error = errno;
    }
    if (directory_error != 0) {
        std::cerr << "NotesService: failed to durably commit notes rename: "
                  << std::strerror(directory_error) << '\n';
        return false;
    }

    return true;
}

} // namespace realmheart::services
