#include "services/NotesService.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

namespace realmheart::services {
namespace {

std::filesystem::path default_notes_path() {
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
    std::error_code ec;
    std::filesystem::create_directories(notes_path_.parent_path(), ec);
    if (ec) {
        std::cerr << "NotesService: failed to create notes directory: " << ec.message() << '\n';
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
    if (worker_.joinable()) {
        worker_.join();
    }
}

void NotesService::load_from_disk() {
    std::lock_guard lock(mutex_);

    std::ifstream file(notes_path_, std::ios::binary);
    if (!file.is_open()) {
        cached_content_.clear();
        return;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    cached_content_ = buffer.str();
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
    std::lock_guard lock(mutex_);
    if (write_atomically_locked()) {
        dirty_ = false;
    }
}

void NotesService::worker_loop() {
    std::unique_lock lock(mutex_);

    while (true) {
        cv_.wait(lock, [this] { return stopping_ || dirty_; });

        if (stopping_) {
            if (dirty_) {
                static_cast<void>(write_atomically_locked());
                dirty_ = false;
            }
            return;
        }

        const auto observed_generation = edit_generation_;
        const auto deadline = std::chrono::steady_clock::now() + debounce_;
        const bool interrupted = cv_.wait_until(lock, deadline, [this, observed_generation] {
            return stopping_ || edit_generation_ != observed_generation;
        });

        if (stopping_) {
            if (dirty_) {
                static_cast<void>(write_atomically_locked());
                dirty_ = false;
            }
            return;
        }

        if (interrupted) {
            continue;
        }

        if (dirty_ && write_atomically_locked()) {
            dirty_ = false;
        }
    }
}

bool NotesService::write_atomically_locked() {
    const auto temporary_path = std::filesystem::path(notes_path_.string() + ".tmp");

    {
        std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            std::cerr << "NotesService: failed to open temporary notes file\n";
            return false;
        }

        file.write(cached_content_.data(), static_cast<std::streamsize>(cached_content_.size()));
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
