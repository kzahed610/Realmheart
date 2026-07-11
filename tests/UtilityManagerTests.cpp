#include "services/UtilityManager.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <memory>

class MockUtilityExecutor : public realmheart::services::IUtilityExecutor {
public:
    std::vector<std::vector<std::string>> background_calls;
    std::vector<std::vector<std::string>> capture_calls;
    realmheart::core::CommandResult next_capture_result;
    bool next_background_result = true;

    bool run_background(const std::vector<std::string>& argv) override {
        background_calls.push_back(argv);
        return next_background_result;
    }
    realmheart::core::CommandResult run_capture(const std::vector<std::string>& argv) override {
        capture_calls.push_back(argv);
        return next_capture_result;
    }
};

void test_screenshot_full() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::UtilityManager util(std::move(mock));
    bool result = util.take_screenshot_full("/tmp/test.png");
    if (!result) { std::cerr << "Full screenshot failed\n"; exit(1); }
    if (mock_ptr->background_calls[0] != std::vector<std::string>{"grim", "/tmp/test.png"}) { std::cerr << "Wrong command\n"; exit(1); }
    std::cout << "test_screenshot_full PASSED\n";
}

void test_screenshot_area() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::UtilityManager util(std::move(mock));
    bool result = util.take_screenshot_area("/tmp/area.png");
    if (!result) { std::cerr << "Area screenshot failed\n"; exit(1); }
    if (mock_ptr->background_calls[0][0] != "sh") { std::cerr << "Should use shell\n"; exit(1); }
    std::cout << "test_screenshot_area PASSED\n";
}

void test_ocr_area() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::UtilityManager util(std::move(mock));
    
    bool result = util.extract_text_from_area();
    
    if (!result) { std::cerr << "OCR failed\n"; exit(1); }
    if (mock_ptr->background_calls.size() != 1) { std::cerr << "OCR must be one race-free pipeline\n"; exit(1); }
    if (mock_ptr->background_calls[0][0] != "sh") { std::cerr << "OCR should use one shell pipeline\n"; exit(1); }
    if (mock_ptr->background_calls[0][2].find("tesseract") == std::string::npos) { std::cerr << "Missing tesseract\n"; exit(1); }
    if (mock_ptr->background_calls[0][2].find("trap") == std::string::npos) { std::cerr << "Missing OCR cleanup\n"; exit(1); }
    
    std::cout << "test_ocr_area PASSED\n";
}

void test_clipboard_copy() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    realmheart::services::UtilityManager util(std::move(mock));
    bool result = util.copy_to_clipboard("Hello");
    if (!result) { std::cerr << "Copy failed\n"; exit(1); }
    std::cout << "test_clipboard_copy PASSED\n";
}

void test_clipboard_paste() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    realmheart::core::CommandResult res;
    res.output = "Pasted Text";
    res.status = realmheart::core::CommandStatus::Exited;
    res.exit_code = 0;
    mock->next_capture_result = res;
    realmheart::services::UtilityManager util(std::move(mock));
    if (util.paste_from_clipboard() != "Pasted Text") { std::cerr << "Paste failed\n"; exit(1); }
    std::cout << "test_clipboard_paste PASSED\n";
}

void test_launch_wofi() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    realmheart::services::UtilityManager util(std::move(mock));
    if (!util.launch_wofi()) { std::cerr << "Wofi failed\n"; exit(1); }
    std::cout << "test_launch_wofi PASSED\n";
}

void test_choose_wallpaper() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::UtilityManager util(std::move(mock));
    if (!util.choose_wallpaper()) { std::cerr << "Wallpaper picker failed\n"; exit(1); }
    const std::vector<std::string> expected{
        "/home/zahed/.config/realmheart/scripts/colors/switchwall.sh"
    };
    if (mock_ptr->background_calls.size() != 1 || mock_ptr->background_calls[0] != expected) {
        std::cerr << "Wallpaper picker must invoke switchwall without a fake path\n";
        exit(1);
    }
    std::cout << "test_choose_wallpaper PASSED\n";
}

void test_recorder_uses_owned_pid() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-recorder-test";
    const auto pid_file = root / "wf-recorder.pid";
    const auto proc_root = root / "proc";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(proc_root / "4242");

    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::UtilityManager util(std::move(mock), pid_file, proc_root);

    if (!util.start_recording("/tmp/owned recording.mp4")) {
        std::cerr << "Recorder start failed\n";
        exit(1);
    }
    const auto& start = mock_ptr->background_calls.back();
    if (start.size() != 6 || start[0] != "sh" || start[1] != "-c" ||
        start[4] != pid_file.string() || start[5] != "/tmp/owned recording.mp4") {
        std::cerr << "Recorder start must write an ownership PID file with argv-safe parameters\n";
        exit(1);
    }

    std::ofstream(proc_root / "4242/comm") << "wf-recorder\n";
    std::ofstream stat(proc_root / "4242/stat");
    stat << "4242 (wf-recorder) S";
    for (int field = 4; field <= 21; ++field) stat << " 0";
    stat << " 98765\n";
    stat.close();
    std::ofstream(pid_file) << "4242 98765\n";

    if (!util.stop_recording()) {
        std::cerr << "Owned recorder stop failed\n";
        exit(1);
    }
    const std::vector<std::string> expected{"kill", "-INT", "4242"};
    if (mock_ptr->background_calls.back() != expected) {
        std::cerr << "Recorder stop must signal only the owned PID\n";
        exit(1);
    }
    for (const auto& call : mock_ptr->background_calls) {
        if (!call.empty() && call[0] == "pkill") {
            std::cerr << "Recorder stop must never use pkill\n";
            exit(1);
        }
    }

    std::filesystem::remove_all(root);
    std::cout << "test_recorder_uses_owned_pid PASSED\n";
}

void test_recorder_rejects_stale_pid_file() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-recorder-stale-test";
    const auto pid_file = root / "wf-recorder.pid";
    const auto proc_root = root / "proc";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(proc_root / "4242");
    std::ofstream(proc_root / "4242/comm") << "wf-recorder\n";
    std::ofstream stat(proc_root / "4242/stat");
    stat << "4242 (wf-recorder) S";
    for (int field = 4; field <= 21; ++field) stat << " 0";
    stat << " 22222\n";
    stat.close();
    std::ofstream(pid_file) << "4242 11111\n";

    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::UtilityManager util(std::move(mock), pid_file, proc_root);
    if (util.stop_recording()) {
        std::cerr << "Stale recorder PID must be rejected\n";
        exit(1);
    }
    if (!mock_ptr->background_calls.empty()) {
        std::cerr << "Stale PID must not signal any process\n";
        exit(1);
    }
    std::filesystem::remove_all(root);
    std::cout << "test_recorder_rejects_stale_pid_file PASSED\n";
}

int main() {
    test_screenshot_full();
    test_screenshot_area();
    test_ocr_area();
    test_clipboard_copy();
    test_clipboard_paste();
    test_launch_wofi();
    test_choose_wallpaper();
    test_recorder_uses_owned_pid();
    test_recorder_rejects_stale_pid_file();
    std::cout << "All UtilityManager tests PASSED (MOCKED)\n";
    return 0;
}
