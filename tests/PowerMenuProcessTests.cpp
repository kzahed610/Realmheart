#include "ui/powermenu/PowerMenuProcess.hpp"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void wait_for_file(const std::filesystem::path& path) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (std::filesystem::exists(path)) return;
        std::this_thread::sleep_for(10ms);
    }
}

void test_shutdown_is_bounded_and_process_group_aware() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-power-menu-process-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto child_pid_path = root / "child.pid";
    const auto helper = root / "helper.sh";

    {
        std::ofstream script(helper);
        script << "#!/bin/sh\n"
               << "(sleep 30) &\n"
               << "echo $! > '" << child_pid_path.string() << "'\n"
               << "trap '' TERM\n"
               << "while :; do sleep 1; done\n";
    }
    std::filesystem::permissions(
        helper,
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add
    );

    pid_t descendant = -1;
    const auto started = std::chrono::steady_clock::now();
    {
        realmheart::ui::powermenu::PowerMenuProcess process(helper.string());
        process.toggle(0, 0.1, 0.9);
        require(process.running(), "fixture helper must be tracked after launch");
        wait_for_file(child_pid_path);
        std::ifstream child_pid(child_pid_path);
        child_pid >> descendant;
        require(descendant > 0, "fixture must publish a descendant pid");
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(elapsed < 2s, "renderer destruction must remain bounded");

    bool descendant_reaped = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (::kill(descendant, 0) != 0 && errno == ESRCH) {
            descendant_reaped = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    require(descendant_reaped, "renderer shutdown must clean up descendants in its process group");
    std::filesystem::remove_all(root);
}

void test_startup_watchdog_rejects_never_ready_helper() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-power-menu-watchdog-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto helper = root / "helper.sh";
    {
        std::ofstream script(helper);
        script << "#!/bin/sh\n"
               << "while :; do sleep 1; done\n";
    }
    std::filesystem::permissions(
        helper,
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add
    );

    realmheart::ui::powermenu::PowerMenuProcess process(helper.string());
    process.toggle(0, 0.1, 0.9);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (process.running() && std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, FALSE);
        }
        std::this_thread::sleep_for(10ms);
    }
    require(!process.running(), "never-ready helper must be terminated by startup watchdog");
    std::filesystem::remove_all(root);
}

void test_startup_accepts_split_ready_message() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-power-menu-ready-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto helper = root / "helper.sh";
    {
        std::ofstream script(helper);
        script << "#!/bin/sh\n"
               << "printf re >&0\n"
               << "sleep 0.05\n"
               << "printf 'ady\\n' >&0\n"
               << "while :; do sleep 1; done\n";
    }
    std::filesystem::permissions(
        helper,
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add
    );

    {
        realmheart::ui::powermenu::PowerMenuProcess process(helper.string());
        process.toggle(0, 0.1, 0.9);
        const auto deadline = std::chrono::steady_clock::now() + 1800ms;
        while (std::chrono::steady_clock::now() < deadline) {
            while (g_main_context_pending(nullptr)) {
                g_main_context_iteration(nullptr, FALSE);
            }
            std::this_thread::sleep_for(10ms);
        }
        require(process.running(), "split ready message must satisfy startup watchdog");
    }
    std::filesystem::remove_all(root);
}

void test_graceful_close_finishes_before_escalation() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-power-menu-graceful-close-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto completed_path = root / "completed";
    const auto helper = root / "helper.sh";
    {
        std::ofstream script(helper);
        script << "#!/bin/sh\n"
               << "IFS= read -r command || exit 1\n"
               << "if [ \"$command\" = close ]; then\n"
               << "  sleep 0.9\n"
               << "  : > '" << completed_path.string() << "'\n"
               << "  exit 0\n"
               << "fi\n"
               << "exit 1\n";
    }
    std::filesystem::permissions(
        helper,
        std::filesystem::perms::owner_exec |
            std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::add
    );

    {
        realmheart::ui::powermenu::PowerMenuProcess process(helper.string());
        process.toggle(0, 0.1, 0.9);
        require(process.running(), "graceful-close fixture must be tracked after launch");
    }
    require(
        std::filesystem::exists(completed_path),
        "normal close must finish before teardown escalation"
    );
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_shutdown_is_bounded_and_process_group_aware();
    test_startup_watchdog_rejects_never_ready_helper();
    test_startup_accepts_split_ready_message();
    test_graceful_close_finishes_before_escalation();
    std::cout << "Power Menu process tests passed\n";
    return 0;
}
