#include "services/NotesService.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cassert>

void test_notes_persistence() {
    std::cout << "Testing NotesService persistence..." << std::endl;
    
    realmheart::services::NotesService service;
    std::string test_content = "Lumen's secret notes: don't touch the purple fire.";
    
    // Test set and save
    service.set_content(test_content);
    service.save();
    
    // Verify file exists and has content
    std::string path = service.get_file_path();
    std::ifstream file(path);
    std::string file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
    if (file_content != test_content) {
        std::cerr << "FAIL: File content mismatch. Expected: " << test_content << " Got: " << file_content << std::endl;
        exit(1);
    }
    
    // Test reload
    realmheart::services::NotesService service2;
    if (service2.get_content() != test_content) {
        std::cerr << "FAIL: Reloaded content mismatch. Expected: " << test_content << " Got: " << service2.get_content() << std::endl;
        exit(1);
    }
    
    std::cout << "NotesService persistence PASSED" << std::endl;
}

int main() {
    test_notes_persistence();
    return 0;
}
