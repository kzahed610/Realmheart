#pragma once

#include <string>
#include <filesystem>
#include <mutex>

namespace realmheart::services {

class NotesService {
public:
    NotesService();
    ~NotesService() = default;

    // Get the current content of the notes
    std::string get_content();

    // Set the content and persist it to disk
    void set_content(const std::string& content);

    // Force save current state to disk
    void save();

    // Path to the notes file
    std::string get_file_path() const { return notes_path_; }

private:
    std::string notes_path_;
    std::string cached_content_;
    std::mutex mutex_;

    void load_from_disk();
};

} // namespace realmheart::services
