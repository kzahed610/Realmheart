#include "services/NotesService.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace realmheart::services {

NotesService::NotesService() {
    // Store notes in a simple plaintext file in ~/.config/realmheart/notes.txt
    notes_path_ = std::string(getenv("HOME")) + "/.config/realmheart/notes.txt";
    
    // Ensure directory exists
    std::filesystem::create_directories(std::filesystem::path(notes_path_).parent_path());
    
    load_from_disk();
}

void NotesService::load_from_disk() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!std::filesystem::exists(notes_path_)) {
        cached_content_ = "";
        return;
    }

    std::ifstream file(notes_path_);
    if (!file.is_open()) {
        cached_content_ = "";
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    cached_content_ = buffer.str();
}

std::string NotesService::get_content() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_content_;
}

void NotesService::set_content(const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    cached_content_ = content;
}

void NotesService::save() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::ofstream file(notes_path_, std::ios::trunc);
    if (!file.is_open()) {
        return;
    }
    file << cached_content_;
}

} // namespace realmheart::services
