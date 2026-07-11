#include "LauncherService.hpp"
#include <algorithm>
#include <iostream>
#include <cctype>
#include <gio/gio.h>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace realmheart::services {

namespace fs = std::filesystem;

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

bool SystemLauncherCommandExecutor::run_command(std::string_view command) {
    auto arguments = launcher_command_argv(command);
    std::vector<gchar*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);

    GError* error = nullptr;
    const gboolean spawned = g_spawn_async(
        nullptr,
        argv.data(),
        nullptr,
        G_SPAWN_SEARCH_PATH,
        nullptr,
        nullptr,
        nullptr,
        &error
    );
    if (error != nullptr) {
        std::cerr << "Failed to launch terminal command: " << error->message << "\n";
        g_error_free(error);
    }
    return spawned == TRUE;
}

LauncherService::LauncherService(std::unique_ptr<ILauncherCommandExecutor> command_executor)
    : command_executor_(std::move(command_executor)) {
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

    // 2. Index Custom Actions
    std::string actions_path = "/home/zahed/.config/realmheart/actions/";
    if (fs::exists(actions_path) && fs::is_directory(actions_path)) {
        for (const auto& entry : fs::directory_iterator(actions_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string action_name = filename.substr(0, filename.find_last_of('.'));
                
                LauncherResult res;
                res.kind = LauncherResultKind::Action;
                res.id = entry.path().string();
                res.title = action_name;
                res.subtitle = "Custom Action";
                res.icon_name = "system-run";
                index_.push_back(res);
            }
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
    std::transform(title_lower.begin(), title_lower.end(), title_lower.begin(), ::tolower);
    std::string query_lower = std::string(query);
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

    if (title_lower == query_lower) return 1000;
    if (title_lower.find(query_lower) == 0) return 500;
    if (title_lower.find(" " + query_lower) != std::string::npos) return 250;
    if (title_lower.find(query_lower) != std::string::npos) return 100;

    return 0;
}

std::vector<LauncherResult> LauncherService::search(std::string_view query, std::size_t limit) const {
    if (query.empty()) return {};

    const bool has_shell_prefix = query.front() == '$';
    bool is_explicit_command = has_shell_prefix;
    if (query[0] == '/' || query[0] == '~' || query.find(' ') != std::string::npos) {
        is_explicit_command = true;
    }

    std::vector<ScoredResult> scored;
    for (std::size_t i = 0; i < index_.size(); ++i) {
        int score = calculate_score(index_[i], query);
        if (score > 0) {
            scored.push_back({i, score});
        }
    }

    std::sort(scored.begin(), scored.end(), std::greater<>());

    std::vector<LauncherResult> results;
    std::string command(query);
    if (has_shell_prefix) {
        command.erase(command.begin());
        const auto first = command.find_first_not_of(" \t");
        command = first == std::string::npos ? std::string{} : command.substr(first);
    }
    LauncherResult command_result;
    command_result.kind = LauncherResultKind::Command;
    command_result.id = command;
    command_result.title = "Execute Command";
    command_result.subtitle = command;
    command_result.icon_name = "utilities-terminal";

    if (is_explicit_command) {
        results.push_back(command_result);
    }

    for (std::size_t i = 0; i < std::min(scored.size(), limit); ++i) {
        results.push_back(index_[scored[i].index]);
    }

    if (!is_explicit_command) {
        results.push_back(command_result);
    }

    return results;
}

bool LauncherService::activate(const LauncherResult& result) {
    if (result.kind == LauncherResultKind::Application) {
        GList* apps = g_app_info_get_all();
        if (apps) {
            for (GList* iter = apps; iter != nullptr; iter = g_list_next(iter)) {
                GAppInfo* app = static_cast<GAppInfo*>(iter->data);
                if (result.id == g_app_info_get_id(app)) {
                    GError* error = nullptr;
                    bool success = g_app_info_launch(app, nullptr, nullptr, &error);
                    if (error) {
                        std::cerr << "Failed to launch app " << result.id << ": " << error->message << "\n";
                        g_error_free(error);
                    }
                    g_list_free_full(apps, g_object_unref);
                    return success;
                }
            }
            g_list_free_full(apps, g_object_unref);
        }
    } else if (result.kind == LauncherResultKind::Command) {
        if (result.id.find_first_not_of(" \t\n\r") == std::string::npos) {
            return false;
        }
        return command_executor_->run_command(result.id);
    } else if (result.kind == LauncherResultKind::Action) {
        gchar* argv[] = { (gchar*)"/bin/bash", (gchar*)result.id.c_str(), nullptr };
        GError* error = nullptr;
        bool success = g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error);
        if (error) {
            std::cerr << "Failed to launch action " << result.id << ": " << error->message << "\n";
            g_error_free(error);
        }
        return success;
    } else if (result.kind == LauncherResultKind::Emoji) {
        // Copy the emoji to clipboard
        gchar* argv[] = { (gchar*)"wl-copy", (gchar*)result.id.c_str(), nullptr };
        GError* error = nullptr;
        bool success = g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error);
        if (error) {
            std::cerr << "Failed to copy emoji " << result.id << ": " << error->message << "\n";
            g_error_free(error);
        }
        return success;
    } else if (result.kind == LauncherResultKind::Clipboard) {
        // This is an activate call for a clipboard entry.
        // We use cliphist print to get the content and copy it back to the clipboard.
        // For now, we assume the ID is the index in the lauchner service's current search results
        // We'll implement the search for cliphist later in search()
        return true;
    }
    return false;
}

} // namespace realmheart::services
