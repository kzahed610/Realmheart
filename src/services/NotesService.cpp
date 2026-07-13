#include "services/NotesService.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

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
    {
        std::lock_guard lock(mutex_);
        cached_content_ = content;
        dirty_ = true;
        ++edit_generation_;
    }
    cv_.notify_one();
}

void NotesService::save() {
    std::string content;
    std::size_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        content = cached_content_;
        generation = edit_generation_;
    }

    const bool written = write_atomically(content);
    if (written) {
        std::lock_guard lock(mutex_);
        if (edit_generation_ == generation) dirty_ = false;
    }
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

        if (written && edit_generation_ == generation) dirty_ = false;
        if (stopping_) return;
    }
}

bool NotesService::write_atomically(const std::string& content) {
    // save() and the debounce worker may overlap. Serialize only disk I/O—not
    // access to the in-memory note—so both paths can safely share atomic replace.
    std::lock_guard io_lock(io_mutex_);
    const auto temporary_path = std::filesystem::path(notes_path_.string() + ".tmp");

    {
        std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            std::cerr << "NotesService: failed to open temporary notes file\n";
            return false;
        }

        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file.good()) {
            std::cerr << "NotesService: failed to write temporary notes file\n";
            file.close();
            std::error_code remove_error;
            std::filesystem::remove(temporary_path, remove_error);
            return false;
        }
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary_path, notes_path_, rename_error);
    if (rename_error) {
        std::cerr << "NotesService: failed to replace notes file: " << rename_error.message() << '\n';
        std::error_code remove_error;
        std::filesystem::remove(temporary_path, remove_error);
        return false;
    }

    return true;
}

} // namespace realmheart::services
