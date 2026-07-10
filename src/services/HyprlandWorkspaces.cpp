#include "services/HyprlandWorkspaces.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>

namespace realmheart::services {
namespace {

std::optional<int> parse_int_field(std::string_view object, std::string_view key) {
    const std::regex pattern("\\\"" + std::string(key) + "\\\"\\s*:\\s*(-?[0-9]+)");
    std::cmatch match;
    if (!std::regex_search(object.data(), object.data() + object.size(), match, pattern)) return std::nullopt;

    int value = 0;
    const auto text = match[1].str();
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<std::string> parse_string_field(std::string_view object, std::string_view key) {
    const std::regex pattern("\\\"" + std::string(key) + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::cmatch match;
    if (!std::regex_search(object.data(), object.data() + object.size(), match, pattern)) return std::nullopt;
    return match[1].str();
}

std::optional<int> parse_active_id(std::string_view json) {
    return parse_int_field(json, "id");
}

std::vector<WorkspaceState> parse_workspaces(std::string_view json, int active_id) {
    std::vector<WorkspaceState> workspaces;
    std::set<int> seen;
    std::size_t cursor = 0;

    while (true) {
        const auto begin = json.find('{', cursor);
        if (begin == std::string_view::npos) break;
        const auto end = json.find('}', begin);
        if (end == std::string_view::npos) break;

        const auto object = json.substr(begin, end - begin + 1);
        cursor = end + 1;

        const auto id = parse_int_field(object, "id");
        if (!id || *id <= 0 || seen.contains(*id)) continue;

        WorkspaceState workspace;
        workspace.id = *id;
        workspace.name = parse_string_field(object, "name").value_or(std::to_string(*id));
        workspace.windows = parse_int_field(object, "windows").value_or(0);
        workspace.active = *id == active_id;
        seen.insert(*id);
        workspaces.push_back(workspace);
    }

    std::sort(workspaces.begin(), workspaces.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return workspaces;
}

WorkspaceSnapshot unavailable(std::string reason) {
    WorkspaceSnapshot snapshot;
    snapshot.available = false;
    snapshot.error = std::move(reason);
    return snapshot;
}

} // namespace

WorkspaceSnapshot HyprlandWorkspaces::read(const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("hyprctl")) {
        return unavailable("hyprctl not found");
    }

    const auto active = realmheart::core::run_capture({"hyprctl", "activeworkspace", "-j"}, options);
    if (!active.succeeded() || active.output.empty() || active.truncated) {
        return unavailable(realmheart::core::command_failure_detail(active, "hyprctl activeworkspace failed"));
    }

    const auto workspace_result = realmheart::core::run_capture({"hyprctl", "workspaces", "-j"}, options);
    if (!workspace_result.succeeded() || workspace_result.output.empty() || workspace_result.truncated) {
        return unavailable(realmheart::core::command_failure_detail(workspace_result, "hyprctl workspaces failed"));
    }

    return parse(active.output, workspace_result.output);
}

WorkspaceSnapshot HyprlandWorkspaces::parse(std::string_view active_json, std::string_view workspaces_json) {
    const auto active_id = parse_active_id(active_json);
    if (!active_id || *active_id <= 0) {
        return unavailable("unable to parse active workspace id");
    }

    const auto normalized_workspaces = realmheart::core::trim(std::string(workspaces_json));
    if (normalized_workspaces.empty()
        || (normalized_workspaces != "[]" && normalized_workspaces.find('{') == std::string::npos)) {
        return unavailable("unable to parse workspace list");
    }

    WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = *active_id;
    snapshot.workspaces = parse_workspaces(workspaces_json, *active_id);

    const auto has_active = std::any_of(snapshot.workspaces.begin(), snapshot.workspaces.end(), [active_id](const auto& workspace) {
        return workspace.id == *active_id;
    });
    if (!has_active) {
        snapshot.workspaces.push_back({*active_id, std::to_string(*active_id), 0, true});
        std::sort(snapshot.workspaces.begin(), snapshot.workspaces.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
    }

    return snapshot;
}

std::string HyprlandWorkspaces::describe(const WorkspaceSnapshot& snapshot) {
    std::ostringstream out;
    if (!snapshot.available) {
        out << "Hyprland Workspaces: unavailable";
        if (!snapshot.error.empty()) out << " (" << snapshot.error << ')';
        out << '\n';
        return out.str();
    }

    out << "Hyprland Workspaces: active=" << snapshot.active_id << '\n';
    for (const auto& workspace : snapshot.workspaces) {
        out << "  " << workspace.id << ": ";
        if (workspace.active) out << "active";
        else out << "inactive";
        out << ", windows=" << workspace.windows;
        if (!workspace.name.empty() && workspace.name != std::to_string(workspace.id)) {
            out << ", name=" << workspace.name;
        }
        out << '\n';
    }
    return out.str();
}

} // namespace realmheart::services
