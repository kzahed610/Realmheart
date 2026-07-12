#include "core/Diagnostics.hpp"
#include "core/ShellCommand.hpp"
#include "core/ShellControl.hpp"
#include "services/Audio.hpp"
#include "services/Brightness.hpp"
#include "services/HyprlandWorkspaces.hpp"
#include "services/PowerProfiles.hpp"
#include "services/ScopeModules.hpp"
#include "services/RightSidebarServices.hpp"
#include "ui/GtkApp.hpp"
#include "ui/ShellApp.hpp"

#include <charconv>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string get_supported_commands() {
    std::string list = "sidebar-right-toggle, bar-toggle, osd-volume, osd-brightness, lock-session, logout-menu, screenshot-full, screenshot-area, extract-ocr, start-recording, stop-recording, toggle-notes, set-wallpaper, set-wallpaper-path, generate-theme, launch-launcher, quit";
    return list;
}

void print_usage() {
    std::cout << "Realmheart 0.1.0\n"
              << "Usage:\n"
              << "  realmheart --doctor              Probe host dependencies and live service state\n"
              << "  realmheart --list-modules        Print confirmed module registry\n"
              << "  realmheart --cycle-power-profile Cycle battery-saver/balanced/performance\n"
              << "  realmheart --workspace-status   Print Hyprland workspace snapshot\n"
              << "  realmheart --right-sidebar-status Print right sidebar service report\n"
              << "  realmheart --shell               Run the persistent Realmheart shell\n"
              << "  realmheart --command NAME        Send a command to the running shell\n"
              << "  realmheart --sidebar [--timeout N] Show the right sidebar MVP layer surface\n"
              << "  realmheart --bar [--timeout N]     Show the safe vertical bar MVP layer surface\n"
              << "  realmheart --test-layer [--timeout N] Show a temporary GTK layer-shell test surface\n"
              << "  realmheart --help                Show this help\n";
}

bool parse_positive_int(std::string_view text, int& value) {
    int parsed = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed <= 0) {
        return false;
    }

    value = parsed;
    return true;
}

int parse_timeout_option(int argc, char** argv, int& timeout_seconds, std::string_view command_name) {
    timeout_seconds = 5;

    for (int index = 2; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument != "--timeout") {
            std::cerr << "Unknown " << command_name << " argument: " << argument << "\n";
            return 2;
        }

        if (index + 1 >= argc) {
            std::cerr << "--timeout requires a positive integer value\n";
            return 2;
        }

        if (!parse_positive_int(argv[++index], timeout_seconds)) {
            std::cerr << "Invalid --timeout value: " << argv[index] << "\n";
            return 2;
        }
    }

    return 0;
}

int doctor() {
    std::cout << realmheart::core::format_dependency_report(realmheart::core::collect_dependency_checks()) << '\n';

    if (auto brightness = realmheart::services::Brightness::read()) {
        std::cout << "Brightness: " << brightness->current << '/' << brightness->maximum
                  << " (" << std::fixed << std::setprecision(1) << brightness->percent << "%)\n";
    } else {
        std::cout << "Brightness: unavailable\n";
    }

    if (auto audio = realmheart::services::Audio::read_default_sink()) {
        std::cout << "Audio: " << audio->raw_status << '\n';
    } else {
        std::cout << "Audio: unavailable\n";
    }

    if (auto profile = realmheart::services::PowerProfiles::current()) {
        std::cout << "Power profile: " << *profile
                  << " (next: " << realmheart::services::PowerProfiles::next_after(*profile) << ")\n";
    } else {
        std::cout << "Power profile: unavailable\n";
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "--help";

    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }

    if (command == "--doctor") return doctor();

    if (command == "--shell") {
        if (argc != 2) {
            std::cerr << "--shell does not accept additional arguments\n";
            return 2;
        }
        return realmheart::ui::run_shell();
    }

    if (command == "--command") {
        if (argc < 3) {
            std::cerr << "--command requires at least one command name\n"
                      << "Supported: " << get_supported_commands() << "\n";
            return 2;
        }

        const auto shell_command = realmheart::core::parse_shell_command(argv[2]);
        if (!shell_command) {
            std::cerr << "Unknown shell command: " << argv[2] << '\n'
                      << "Supported: " << get_supported_commands() << "\n";
            return 2;
        }

        std::string_view shell_argument;
        if (realmheart::core::shell_command_requires_argument(*shell_command)) {
            if (argc < 4 || std::string_view(argv[3]).empty()) {
                std::cerr << argv[2] << " requires an argument\n";
                return 2;
            }
            shell_argument = argv[3];
        }

        const auto result = realmheart::core::send_shell_command(*shell_command, shell_argument);
        switch (result) {
        case realmheart::core::ShellControlResult::Delivered:
            return 0;
        case realmheart::core::ShellControlResult::NotRunning:
            std::cerr << "Realmheart shell is not running\n";
            return 1;
        case realmheart::core::ShellControlResult::RegistrationFailed:
            return 3;
        case realmheart::core::ShellControlResult::ActionUnavailable:
            std::cerr << "Running Realmheart shell does not expose command: " << argv[2] << '\n';
            return 4;
        case realmheart::core::ShellControlResult::InvalidArgument:
            std::cerr << "Invalid or missing argument for command: " << argv[2] << '\n';
            return 2;
        }
    }

    if (command == "--list-modules") {
        const auto registry = realmheart::services::build_confirmed_module_registry();
        std::cout << registry.describe();
        return 0;
    }

    if (command == "--cycle-power-profile") {
        auto next = realmheart::services::PowerProfiles::cycle();
        if (!next) {
            std::cerr << "Unable to cycle power profile; is powerprofilesctl available and working?\n";
            return 1;
        }
        std::cout << "Power profile set to " << *next << '\n';
        return 0;
    }

    if (command == "--workspace-status") {
        const auto snapshot = realmheart::services::HyprlandWorkspaces::read();
        std::cout << realmheart::services::HyprlandWorkspaces::describe(snapshot);
        return 0;
    }

    if (command == "--right-sidebar-status") {
        realmheart::services::RightSidebarServices services_report;
        services_report.printReport();
        return 0;
    }

    if (command == "--bar") {
        int timeout_seconds = 5;
        const int parse_status = parse_timeout_option(argc, argv, timeout_seconds, "--bar");
        if (parse_status != 0) {
            print_usage();
            return parse_status;
        }

        return realmheart::ui::run_bar(timeout_seconds);
    }

    if (command == "--sidebar") {
        int timeout_seconds = 5;
        const int parse_status = parse_timeout_option(argc, argv, timeout_seconds, "--sidebar");
        if (parse_status != 0) {
            print_usage();
            return parse_status;
        }

        return realmheart::ui::run_sidebar(timeout_seconds);
    }

    if (command == "--test-layer") {
        int timeout_seconds = 5;
        const int parse_status = parse_timeout_option(argc, argv, timeout_seconds, "--test-layer");
        if (parse_status != 0) {
            print_usage();
            return parse_status;
        }

        return realmheart::ui::run_test_layer(timeout_seconds);
    }

    std::cerr << "Unknown argument: " << command << "\n";
    print_usage();
    return 2;
}
