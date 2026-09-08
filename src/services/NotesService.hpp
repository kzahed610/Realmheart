#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace realmheart::services {

enum class NotesSaveState {
    Saved,
    Pending,
    Failed,
    DurabilityUncertain,
    LoadFailed,
    Rejected,
};

class NotesService {
public:
    struct State;
    static constexpr std::size_t max_note_bytes = 1024 * 1024;
    using SaveStateCallback = std::function<void(NotesSaveState)>;

    NotesService();
    explicit NotesService(
        std::filesystem::path notes_path,
        std::chrono::milliseconds debounce = std::chrono::milliseconds{350}
    );
    ~NotesService();

    NotesService(const NotesService&) = delete;
    NotesService& operator=(const NotesService&) = delete;

    std::string get_content() const;

    // Accepts valid UTF-8 text up to max_note_bytes and schedules one
    // debounced atomic disk write. Returns false without changing the note when
    // validation or an unacknowledged load failure rejects the edit.
    bool set_content(const std::string& content);

    // Explicitly permits replacing content after a load failure. No content is
    // changed by this call; it only records user acknowledgement.
    bool acknowledge_load_failure();

    // Flushes the current generation immediately. A stale snapshot is never
    // committed over a newer edit.
    bool save();

    void set_save_state_callback(SaveStateCallback callback);
    [[nodiscard]] NotesSaveState save_state() const;

    std::string get_file_path() const;

private:
    std::shared_ptr<State> state_;
    std::thread worker_;

    void load_from_disk();
    static void worker_loop(std::shared_ptr<State> state);
};

} // namespace realmheart::services
