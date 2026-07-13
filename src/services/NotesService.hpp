#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace realmheart::services {

class NotesService {
public:
    NotesService();
    explicit NotesService(
        std::filesystem::path notes_path,
        std::chrono::milliseconds debounce = std::chrono::milliseconds{350}
    );
    ~NotesService();

    NotesService(const NotesService&) = delete;
    NotesService& operator=(const NotesService&) = delete;

    std::string get_content();

    // Updates the in-memory note immediately and schedules one debounced,
    // atomic disk write. Repeated edits coalesce into the latest content.
    void set_content(const std::string& content);

    // Flushes the current content immediately using atomic replacement.
    void save();

    std::string get_file_path() const { return notes_path_.string(); }

private:
    std::filesystem::path notes_path_;
    std::string cached_content_;
    std::chrono::milliseconds debounce_;
    std::mutex mutex_;
    std::mutex io_mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool dirty_ = false;
    bool stopping_ = false;
    std::size_t edit_generation_ = 0;

    void load_from_disk();
    void worker_loop();
    bool write_atomically(const std::string& content);
};

} // namespace realmheart::services
