#include "LauncherService.hpp"
#include "core/Command.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <cctype>
#include <gio/gio.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace realmheart::services {

namespace fs = std::filesystem;

namespace {

fs::path actions_directory() {
    if (const char* config = std::getenv("XDG_CONFIG_HOME");
        config != nullptr && *config != '\0') {
        return fs::path(config) / "realmheart/actions";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".config/realmheart/actions";
    }
    return fs::temp_directory_path() / "realmheart/actions";
}

char ascii_lower(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool running_inside_systemd_unit() {
    const char* invocation_id = std::getenv("INVOCATION_ID");
    return invocation_id != nullptr && *invocation_id != '\0';
}

std::string trim_copy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\n\r");
    return std::string(value.substr(first, last - first + 1));
}

bool contains_shell_syntax(std::string_view value) {
    constexpr std::string_view syntax = "|&;<>(){}$`\n\r";
    return value.find_first_of(syntax) != std::string_view::npos;
}

std::string first_command_token(std::string_view command) {
    const auto first = command.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto end = command.find_first_of(" \t", first);
    return std::string(command.substr(first, end == std::string_view::npos ? command.size() - first : end - first));
}

bool is_valid_plain_command(std::string_view command) {
    if (command.empty() || contains_shell_syntax(command)) return false;
    const std::string executable = first_command_token(command);
    if (executable.empty()) return false;

    if (executable.find('/') != std::string::npos) {
        std::error_code error;
        const fs::path path(executable);
        return fs::is_regular_file(path, error) && !error;
    }
    return realmheart::core::command_exists(executable);
}

} // namespace

std::vector<std::string> launcher_command_argv(std::string_view command) {
    const auto first = command.find_first_not_of(" \t\n\r");
    const std::string_view trimmed = first == std::string_view::npos ? std::string_view{} : command.substr(first);
    const bool needs_terminal = trimmed == "sudo" ||
        (trimmed.starts_with("sudo") && trimmed.size() > 4 && std::isspace(static_cast<unsigned char>(trimmed[4])));

    std::vector<std::string> argv;
    if (needs_terminal) argv.emplace_back("kitty");
    argv.emplace_back("fish");
    argv.emplace_back("-C");
    argv.emplace_back(command);
    return argv;
}

std::vector<std::string> launcher_application_argv(std::string_view desktop_id) {
    if (desktop_id.empty()) return {};
    return {"gtk4-launch", std::string(desktop_id)};
}

std::vector<std::string> launcher_scoped_argv(const std::vector<std::string>& argv) {
    if (argv.empty() || argv.front().empty()) return {};

    std::vector<std::string> scoped{
        "systemd-run",
        "--user",
        "--scope",
        "--quiet",
        "--collect",
        "--slice=app.slice",
        "--",
    };
    scoped.insert(scoped.end(), argv.begin(), argv.end());
    return scoped;
}

bool SystemLauncherProcessExecutor::run(const std::vector<std::string>& argv) {
    if (argv.empty() || argv.front().empty()) return false;

    // Applications launched by a shell service must not remain in that
    // service's cgroup. Otherwise systemd's default KillMode=control-group
    // terminates them whenever Realmheart stops or restarts. A transient user
    // scope gives each launch an independent lifetime while preserving argv.
    if (running_inside_systemd_unit() && realmheart::core::command_exists("systemd-run")) {
        return realmheart::core::run_background(launcher_scoped_argv(argv));
    }

    // Manually launched Realmheart instances and non-systemd systems do not
    // have a shell service cgroup whose shutdown can take the application down.
    return realmheart::core::run_background(argv);
}

bool SystemLauncherCommandExecutor::run_command(std::string_view command) {
    const auto arguments = launcher_command_argv(command);
    SystemLauncherProcessExecutor executor;
    return executor.run(arguments);
}

LauncherService::LauncherService(
    std::unique_ptr<ILauncherCommandExecutor> command_executor,
    std::unique_ptr<ILauncherProcessExecutor> process_executor
) : command_executor_(std::move(command_executor)),
    process_executor_(std::move(process_executor)) {
    refresh_index();
}

void LauncherService::refresh_index() {
    index_.clear();

    // 1. Index Desktop Applications
    GList* apps = g_app_info_get_all();
    if (apps) {
        for (GList* iter = apps; iter != nullptr; iter = g_list_next(iter)) {
            GAppInfo* app = static_cast<GAppInfo*>(iter->data);
            if (!g_app_info_should_show(app)) continue;

            LauncherResult res;
            res.kind = LauncherResultKind::Application;
            const char* id = g_app_info_get_id(app);
            const char* name = g_app_info_get_name(app);
            if (id == nullptr || name == nullptr) continue;
            res.id = id;
            res.title = name;
            res.subtitle = g_app_info_get_display_name(app) ? g_app_info_get_display_name(app) : "";
            
            GIcon* icon = g_app_info_get_icon(app);
            if (icon) {
                if (G_TYPE_CHECK_INSTANCE_TYPE(icon, G_TYPE_THEMED_ICON)) {
                    const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
                    if (names && names[0]) {
                        res.icon_name = names[0];
                    }
                }
            }
            index_.push_back(res);
        }
        g_list_free_full(apps, g_object_unref);
    }

    // 2. Index Custom Actions from the user's XDG config directory.
    const fs::path actions_path = actions_directory();
    std::error_code filesystem_error;
    if (fs::is_directory(actions_path, filesystem_error) && !filesystem_error) {
        fs::directory_iterator iterator(actions_path, filesystem_error);
        const fs::directory_iterator end;
        for (; !filesystem_error && iterator != end; iterator.increment(filesystem_error)) {
            const auto& entry = *iterator;
            if (!entry.is_regular_file(filesystem_error) || filesystem_error) continue;
            const std::string action_name = entry.path().stem().string();

            LauncherResult res;
            res.kind = LauncherResultKind::Action;
            res.id = entry.path().string();
            res.title = action_name;
            res.subtitle = "Custom Action";
            res.icon_name = "system-run";
            index_.push_back(std::move(res));
        }
    }

    // 3. Index Emojis ( loading from a simple text file for stability)
    // We assume the system has a common emoji list or we provide a basic one.
    // For now, we'll index a small set of common ones.
    std::vector<std::pair<std::string, std::string>> common_emojis = {
        {"smile", "😊"}, {"laugh", "😂"}, {"heart", "❤️"}, {"fire", "🔥"},
        {"thumbsup", "👍"}, {"thumbsdown", "👎"}, {"cry", "😭"}, {"skull", "💀"},
        {"star", "⭐"}, {"rocket", "🚀"}, {"sparkles", "✨"}, {"thinking", "🤔"}
    };
    for (const auto& emoji : common_emojis) {
        LauncherResult res;
        res.kind = LauncherResultKind::Emoji;
        res.id = emoji.second;
        res.title = emoji.first;
        res.subtitle = "Emoji";
        res.icon_name = "emoji";
        index_.push_back(res);
    }
}

void LauncherService::set_mock_index(std::vector<LauncherResult> index) {
    index_ = std::move(index);
}

int LauncherService::calculate_score(const LauncherResult& res, std::string_view query) const {
    if (query.empty()) return 0;

    std::string title_lower = res.title;
    std::transform(title_lower.begin(), title_lower.end(), title_lower.begin(), ascii_lower);
    std::string query_lower = std::string(query);
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ascii_lower);

    if (title_lower == query_lower) return 1000;
    if (title_lower.find(query_lower) == 0) return 500;
    if (title_lower.find(" " + query_lower) != std::string::npos) return 250;
    if (title_lower.find(query_lower) != std::string::npos) return 100;

    return 0;
}

std::vector<LauncherResult> LauncherService::recommendations(std::size_t limit) const {
    std::vector<LauncherResult> results;
    if (limit == 0) return results;

    for (const auto& item : index_) {
        if (item.kind == LauncherResultKind::Application ||
            item.kind == LauncherResultKind::Action) {
            results.push_back(item);
        }
    }

    std::stable_sort(results.begin(), results.end(), [](const auto& left, const auto& right) {
        if (left.kind != right.kind) {
            return left.kind == LauncherResultKind::Application;
        }
        std::string left_title = left.title;
        std::string right_title = right.title;
        std::transform(left_title.begin(), left_title.end(), left_title.begin(), ascii_lower);
        std::transform(right_title.begin(), right_title.end(), right_title.begin(), ascii_lower);
        return left_title < right_title;
    });

    if (results.size() > limit) results.resize(limit);
    return results;
}

std::vector<LauncherResult> LauncherService::search(std::string_view query, std::size_t limit) const {
    std::vector<LauncherResult> results;
    if (limit == 0) return results;

    const std::string trimmed = trim_copy(query);
    if (trimmed.empty()) return results;

    const bool explicit_command = trimmed.front() == '>' || trimmed.front() == '$';
    std::string searchable = trimmed;
    if (explicit_command) {
        searchable = trim_copy(std::string_view(trimmed).substr(1));
        if (searchable.empty()) return results;
    }

    std::vector<ScoredResult> scored;
    if (!explicit_command) {
        for (std::size_t i = 0; i < index_.size(); ++i) {
            const int score = calculate_score(index_[i], searchable);
            if (score > 0) scored.push_back({i, score});
        }
        std::sort(scored.begin(), scored.end(), std::greater<>());
    }

    LauncherResult command_result;
    command_result.kind = LauncherResultKind::Command;
    command_result.id = searchable;
    command_result.title = explicit_command ? "Run explicit command" : "Run command";
    command_result.subtitle = searchable;
    command_result.icon_name = "utilities-terminal";

    if (explicit_command) results.push_back(command_result);

    for (const auto& item : scored) {
        if (results.size() >= limit) break;
        results.push_back(index_[item.index]);
    }

    if (!explicit_command && results.size() < limit && is_valid_plain_command(searchable)) {
        results.push_back(std::move(command_result));
    }
    return results;
}

bool LauncherService::activate(const LauncherResult& result) {
    if (result.kind == LauncherResultKind::Application) {
        if (result.id.empty()) return false;
        return process_executor_->run(launcher_application_argv(result.id));
    }

    if (result.kind == LauncherResultKind::Command) {
        if (result.id.find_first_not_of(" \t\n\r") == std::string::npos) return false;
        return command_executor_->run_command(result.id);
    }

    if (result.kind == LauncherResultKind::Action) {
        if (result.id.empty()) return false;
        return process_executor_->run({"/bin/bash", result.id});
    }

    if (result.kind == LauncherResultKind::Emoji) {
        if (result.id.empty()) return false;
        return process_executor_->run({"wl-copy", result.id});
    }

    if (result.kind == LauncherResultKind::Clipboard) {
        if (result.id.empty()) return false;
        return process_executor_->run({
            "sh", "-c", "cliphist decode \"$1\" | wl-copy", "realmheart-clipboard", result.id
        });
    }

    return false;
}

} // namespace realmheart::services
