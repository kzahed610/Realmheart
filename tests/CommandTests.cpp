#include "core/Command.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

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
}

} // namespace

int main() {
    test_timeout_escalates_and_returns_promptly();
    test_output_is_bounded_and_reported();
    test_spawn_failure_is_structured();
    test_cancellation_terminates_child();
    test_failure_detail_is_terminal_safe_and_single_line();
    std::cout << "Command runner tests passed\n";
    return 0;
}
