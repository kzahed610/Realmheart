#include "services/UtilityManager.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <csignal>
#include <utility>

class MockUtilityExecutor : public realmheart::services::IUtilityExecutor {
public:
    std::vector<std::vector<std::string>> background_calls;
    std::vector<std::vector<std::string>> capture_calls;
    std::vector<realmheart::core::CommandOptions> capture_options;
    realmheart::core::CommandResult next_capture_result;
    bool next_background_result = true;
    bool next_signal_result = true;
    std::vector<std::pair<int, int>> signal_calls;

    bool run_background(const std::vector<std::string>& argv) override {
        background_calls.push_back(argv);
        return next_background_result;
    }
    realmheart::core::CommandResult run_capture(
        const std::vector<std::string>& argv,
        const realmheart::core::CommandOptions& options = {}
    ) override {
        capture_calls.push_back(argv);
        capture_options.push_back(options);
        return next_capture_result;
    }
    bool send_signal(int pid, int signal_number) override {
        signal_calls.emplace_back(pid, signal_number);
        return next_signal_result;
    }
};

void test_screenshot_tool_launch() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::UtilityManager util(std::move(mock));

    if (!util.launch_screenshot_tool()) {
        std::cerr << "Screenshot helper launch failed
";
        exit(1);
    }
    if (mock_ptr->background_calls.size() != 1 ||
        mock_ptr->background_calls.front().size() != 1 ||
        std::filesystem::path(mock_ptr->background_calls.front().front()).filename() !=
            "realmheart-screenshot") {
        std::cerr << "Screenshot action must launch realmheart-screenshot directly
";
        exit(1);
    }

    std::cout << "test_screenshot_tool_launch PASSED
";
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

void test_choose_wallpaper_does_not_launch_legacy_script() {
    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::UtilityManager util(std::move(mock));
    if (util.choose_wallpaper()) {
        std::cerr << "Native wallpaper picker is not integrated yet\n";
        exit(1);
    }
    if (!mock_ptr->background_calls.empty() || !mock_ptr->capture_calls.empty()) {
        std::cerr << "Wallpaper selection must not launch a legacy script\n";
        exit(1);
    }
    std::cout << "test_choose_wallpaper_does_not_launch_legacy_script PASSED\n";
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
    if (mock_ptr->signal_calls != std::vector<std::pair<int, int>>{{4242, SIGINT}}) {
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
    if (!mock_ptr->signal_calls.empty()) {
        std::cerr << "Stale PID must not signal any process\n";
        exit(1);
    }
    std::filesystem::remove_all(root);
    std::cout << "test_recorder_rejects_stale_pid_file PASSED\n";
}

void test_generate_colors_uses_new_wallpaper_and_updates_theme() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-matugen-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto image = root / "purple wallpaper.png";
    std::ofstream(image, std::ios::binary) << "fixture";

    auto theme = std::make_shared<realmheart::services::ThemeService>();
    auto mock = std::make_unique<MockUtilityExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::core::CommandResult result;
    result.status = realmheart::core::CommandStatus::Exited;
    result.exit_code = 0;
    result.output = R"json({
        "colors": {
            "dark": {
                "primary": "#aa55ff",
                "secondary": "#55aaff",
                "tertiary": "#ff55aa",
                "background": "#100b18",
                "surface": "#1b1228",
                "surface_container": "#2a1d3b",
                "on_surface": "#f4eaff",
                "on_surface_variant": "#cfbde0",
                "outline": "#887799",
                "error": "#ff6677"
            }
        }
    })json";
    mock->next_capture_result = result;

    realmheart::services::UtilityManager util(theme, std::move(mock));
    if (!util.generate_colors(image.string())) {
        std::cerr << "Matugen palette generation failed\n";
        exit(1);
    }
    if (mock_ptr->capture_calls.size() != 1) {
        std::cerr << "Matugen should run exactly once\n";
        exit(1);
    }
    const auto& command = mock_ptr->capture_calls.front();
    const std::vector<std::string> expected{
        "matugen", "image", image.string(), "--dry-run", "--json", "hex",
        "--old-json-output", "--source-color-index", "0", "--quiet"
    };
    if (command != expected) {
        std::cerr << "Matugen must be invoked directly with quiet JSON output and argv-safe path transport\n";
        exit(1);
    }
    if (mock_ptr->capture_options.empty() ||
        mock_ptr->capture_options.front().max_output_bytes < 128 * 1024) {
        std::cerr << "Matugen capture limits were not configured\n";
        exit(1);
    }
    if (theme->get_palette().get("primary") != "#aa55ff") {
        std::cerr << "ThemeService did not receive the generated palette\n";
        exit(1);
    }

    std::filesystem::remove_all(root);
    std::cout << "test_generate_colors_uses_new_wallpaper_and_updates_theme PASSED\n";
}

int main() {
    test_screenshot_tool_launch();
    test_clipboard_copy();
    test_clipboard_paste();
    test_launch_wofi();
    test_choose_wallpaper_does_not_launch_legacy_script();
    test_generate_colors_uses_new_wallpaper_and_updates_theme();
    test_recorder_uses_owned_pid();
    test_recorder_rejects_stale_pid_file();
    std::cout << "All UtilityManager tests PASSED (MOCKED)\n";
    return 0;
}
