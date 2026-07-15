#include "services/HyprlandWorkspaces.hpp"

#include "core/Command.hpp"
#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <string_view>

namespace realmheart::services {
namespace {

using json = nlohmann::json;

WorkspaceSnapshot unavailable(std::string reason) {
    WorkspaceSnapshot snapshot;
    snapshot.available = false;
    snapshot.error = std::move(reason);
    return snapshot;
}

} // namespace

bool HyprlandWorkspaces::switch_to(
    int workspace_id,
    const realmheart::core::CommandOptions& options
) {
    if (workspace_id <= 0 || !realmheart::core::command_exists("hyprctl")) return false;
    const std::string dispatcher =
        "hl.dsp.focus({ workspace = " + std::to_string(workspace_id) + " })";
    const auto result = realmheart::core::run_capture(
        {"hyprctl", "dispatch", dispatcher},
        options
    );
    return result.succeeded() && result.output.find("error:") == std::string::npos;
}

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

    const auto clients_result = realmheart::core::run_capture({"hyprctl", "clients", "-j"}, options);
    const std::string_view clients_json =
        clients_result.succeeded() && !clients_result.output.empty() && !clients_result.truncated
            ? std::string_view(clients_result.output)
            : std::string_view("[]");

    return parse(active.output, workspace_result.output, clients_json);
}

WorkspaceSnapshot HyprlandWorkspaces::parse(
    std::string_view active_json,
    std::string_view workspaces_json,
    std::string_view clients_json
) {
    try {
        const auto active_document = json::parse(active_json);
        const auto workspace_document = json::parse(workspaces_json);
        if (!active_document.is_object() || !active_document.contains("id") ||
            !active_document["id"].is_number_integer()) {
            return unavailable("unable to parse active workspace id");
        }
        if (!workspace_document.is_array()) {
            return unavailable("unable to parse workspace list");
        }
        const auto clients_document = json::parse(clients_json);
        if (!clients_document.is_array()) {
            return unavailable("unable to parse Hyprland client list");
        }

        const int active_id = active_document["id"].get<int>();
        if (active_id <= 0) return unavailable("unable to parse active workspace id");

        WorkspaceSnapshot snapshot;
        snapshot.available = true;
        snapshot.active_id = active_id;

        std::set<int> seen;
        for (const auto& item : workspace_document) {
            if (!item.is_object() || !item.contains("id") || !item["id"].is_number_integer()) {
                continue;
            }
            const int id = item["id"].get<int>();
            if (id <= 0 || seen.contains(id)) continue;

            WorkspaceState workspace;
            workspace.id = id;
            workspace.name = item.value("name", std::to_string(id));
            workspace.windows = item.value("windows", 0);
            workspace.active = id == active_id;
            seen.insert(id);
            snapshot.workspaces.push_back(std::move(workspace));
        }

        if (!seen.contains(active_id)) {
            snapshot.workspaces.push_back({active_id, std::to_string(active_id), 0, true, {}});
        }

        for (const auto& client : clients_document) {
            if (!client.is_object() || !client.contains("workspace") || !client["workspace"].is_object()) continue;
            const auto& workspace = client["workspace"];
            if (!workspace.contains("id") || !workspace["id"].is_number_integer()) continue;
            const int workspace_id = workspace["id"].get<int>();
            if (workspace_id <= 0) continue;

            auto target = std::find_if(snapshot.workspaces.begin(), snapshot.workspaces.end(), [workspace_id](const auto& item) {
                return item.id == workspace_id;
            });
            if (target == snapshot.workspaces.end()) continue;

            WorkspaceWindow window;
            window.address = client.value("address", std::string{});
            window.app_id = client.value("class", std::string{});
            if (window.app_id.empty()) window.app_id = client.value("initialClass", std::string{});
            if (window.app_id.empty()) window.app_id = client.value("initialTitle", std::string{});
            if (window.app_id.empty()) window.app_id = "Unknown application";
            window.title = client.value("title", std::string{});
            if (window.title.empty()) window.title = client.value("initialTitle", std::string{});
            if (window.title.empty()) window.title = "Untitled window";
            target->window_details.push_back(std::move(window));
            target->windows = std::max(
                target->windows,
                static_cast<int>(target->window_details.size())
            );
        }

        std::sort(snapshot.workspaces.begin(), snapshot.workspaces.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
        return snapshot;
    } catch (const json::exception&) {
        return unavailable("unable to parse Hyprland workspace JSON");
    }
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
        out << "  " << workspace.id << ": "
            << (workspace.active ? "active" : "inactive")
            << ", windows=" << workspace.windows;
        if (!workspace.name.empty() && workspace.name != std::to_string(workspace.id)) {
            out << ", name=" << workspace.name;
        }
        out << '\n';
    }
    return out.str();
}

} // namespace realmheart::services
