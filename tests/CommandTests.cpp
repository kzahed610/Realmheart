#include "core/Command.hpp"
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_timeout_escalates_and_returns_promptly() {
    realmheart::core::CommandOptions options;
    options.deadline = 120ms;
    options.terminate_grace = 60ms;
    options.max_output_bytes = 4096;

    const auto started = std::chrono::steady_clock::now();
    const auto result = realmheart::core::run_capture(
        {"/bin/sh", "-c", "trap '' TERM; printf started; sleep 5"},
        options
    );
    const auto elapsed = std::chrono::steady_clock::now() - started;

    require(result.status == realmheart::core::CommandStatus::TimedOut, "deadline must report TimedOut");
    require(!result.succeeded(), "timed-out command must not succeed");
    require(result.output == "started", "output produced before timeout must be retained");
    require(elapsed < 1500ms, "timeout plus escalation must remain intrinsically bounded");
}

void test_output_is_bounded_and_reported() {
    realmheart::core::CommandOptions options;
    options.deadline = 2s;
    options.max_output_bytes = 64;

    const auto result = realmheart::core::run_capture(
        {"/bin/sh", "-c", "i=0; while [ \"$i\" -lt 200 ]; do printf 0123456789abcdef; i=$((i+1)); done"},
        options
    );

    require(result.succeeded(), "bounded noisy command should still exit successfully");
    require(result.truncated, "discarded output must set the truncation flag");
    require(result.output.size() <= options.max_output_bytes, "captured output must not exceed the configured bound");
}

void test_spawn_failure_is_structured() {
    const auto result = realmheart::core::run_capture({"/definitely/not/a/realmheart-command"});

    require(result.status == realmheart::core::CommandStatus::SpawnFailed, "exec failure must report SpawnFailed");
    require(!result.error.empty(), "spawn failure must include a structured error detail");
    require(
        result.error.find(std::strerror(ENOENT)) != std::string::npos,
        "spawn failure must format the errno captured in the child"
    );
}

void test_cancellation_terminates_child() {
    realmheart::core::CommandOptions options;
    options.deadline = 5s;
    options.terminate_grace = 50ms;
    const auto started = std::chrono::steady_clock::now();
    options.cancelled = [started] {
        return std::chrono::steady_clock::now() - started >= 50ms;
    };

    const auto result = realmheart::core::run_capture({"/bin/sh", "-c", "sleep 5"}, options);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    require(result.status == realmheart::core::CommandStatus::Cancelled, "cancellation must be distinguishable from timeout");
    require(elapsed < 1500ms, "cancelled command must terminate promptly");
}

void test_failure_detail_is_terminal_safe_and_single_line() {
    const std::string unsafe = "\x1b[31mboom\x1b[0m\nsecond\tline\x07";
    require(
        realmheart::core::sanitize_command_detail(unsafe, 80) == "boom second line",
        "failure detail must strip terminal controls and normalize whitespace"
    );
    require(
        realmheart::core::sanitize_command_detail("0123456789", 8) == "01234...",
        "failure detail must have an explicit bounded ellipsis"
    );
    require(realmheart::core::sanitize_command_detail("abcdef", 0).empty(),
            "zero-byte failure detail must be empty");
    require(realmheart::core::sanitize_command_detail("abcdef", 1) == "a",
            "one-byte failure detail must not underflow");
    require(realmheart::core::sanitize_command_detail("abcdef", 2) == "ab",
            "two-byte failure detail must not underflow");
}

void test_background_preserves_shell_script_as_one_argument() {
    const std::string temp_file = "/tmp/realmheart-argv-test.txt";
    std::filesystem::remove(temp_file);

    const std::string payload = "value with spaces; $(not-shell-code)";
    const std::vector<std::string> argv = {
        "/bin/sh",
        "-c",
        "printf '%s' \"$1\" > \"$2\"",
        "realmheart-test",
        payload,
        temp_file
    };

    const bool spawned = realmheart::core::run_background(argv);
    require(spawned, "run_background must report success for valid paths");

    // Poll for completion
    bool found = false;
    for (int i = 0; i < 20; ++i) {
        if (std::filesystem::exists(temp_file)) {
            found = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    require(found, "background process must eventually create output file");

    std::ifstream ifs(temp_file);
    std::string result_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    require(result_content == payload, "argv must be preserved literally; outer shell must not re-interpret tokens");

    std::filesystem::remove(temp_file);
}

void test_background_reports_exec_failure() {
    const bool spawned = realmheart::core::run_background({"/definitely/not/a/realmheart-command"});
    require(!spawned, "run_background must report failure for non-existent executable");
}

void test_background_rejects_empty_argv() {
    require(!realmheart::core::run_background({}), "run_background must reject empty argv");
}

void test_background_does_not_block_for_long_running_process() {
    const auto started = std::chrono::steady_clock::now();
    const bool spawned = realmheart::core::run_background({"/bin/sh", "-c", "sleep 2"});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    require(spawned, "long running process should spawn successfully");
    require(elapsed < 500ms, "run_background must return promptly without waiting for process exit");
}

} // namespace

int main() {
    test_timeout_escalates_and_returns_promptly();
    test_output_is_bounded_and_reported();
    test_spawn_failure_is_structured();
    test_cancellation_terminates_child();
    test_failure_detail_is_terminal_safe_and_single_line();
    test_background_preserves_shell_script_as_one_argument();
    test_background_reports_exec_failure();
    test_background_rejects_empty_argv();
    test_background_does_not_block_for_long_running_process();
    std::cout << "Command runner tests passed\n";
    return 0;
}
