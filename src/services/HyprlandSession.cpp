#include "services/HyprlandSession.hpp"

#include "core/Command.hpp"
#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace realmheart::services {
namespace {

using json = nlohmann::json;

HyprlandSessionSnapshot unavailable(std::string reason) {
    HyprlandSessionSnapshot snapshot;
    snapshot.available = false;
    snapshot.error = std::move(reason);
    return snapshot;
}

std::string normalize_address(std::string_view address) {
    std::string normalized(address);
    if (normalized.empty()) return {};
    if (!normalized.starts_with("0x")) normalized.insert(0, "0x");

    if (normalized.size() <= 2) return {};
    for (std::size_t index = 2; index < normalized.size(); ++index) {
        if (std::isxdigit(static_cast<unsigned char>(normalized[index])) == 0) return {};
        normalized[index] = static_cast<char>(std::tolower(
            static_cast<unsigned char>(normalized[index])
        ));
    }
    return normalized;
}

} // namespace

HyprlandSessionSnapshot HyprlandSession::read(
    const realmheart::core::CommandOptions& options
) {
    if (!realmheart::core::command_exists("hyprctl")) {
        return unavailable("hyprctl not found");
    }

    const auto clients = realmheart::core::run_capture(
        {"hyprctl", "clients", "-j"},
        options
    );
    if (!clients.succeeded() || clients.output.empty() || clients.truncated) {
        return unavailable(realmheart::core::command_failure_detail(
            clients,
            "hyprctl clients failed"
        ));
    }

    const auto active = realmheart::core::run_capture(
        {"hyprctl", "activewindow", "-j"},
        options
    );
    const std::string_view active_json =
        active.succeeded() && !active.output.empty() && !active.truncated
            ? std::string_view(active.output)
            : std::string_view("{}");

    return parse(clients.output, active_json);
}

HyprlandSessionSnapshot HyprlandSession::parse(
    std::string_view clients_json,
    std::string_view active_window_json
) {
    try {
        const auto clients = json::parse(clients_json);
        if (!clients.is_array()) return unavailable("unable to parse Hyprland clients");

        std::string active_address;
        const auto active = json::parse(active_window_json);
        if (active.is_object()) {
            active_address = normalize_address(active.value("address", std::string{}));
        }

        HyprlandSessionSnapshot snapshot;
        snapshot.available = true;
        snapshot.windows.reserve(clients.size());

        for (const auto& client : clients) {
            if (!client.is_object()) continue;
            if (client.contains("mapped") && client["mapped"].is_boolean() &&
                !client["mapped"].get<bool>()) {
                continue;
            }

            HyprlandSessionWindow window;
            window.address = normalize_address(client.value("address", std::string{}));
            if (window.address.empty()) continue;

            window.app_id = client.value("class", std::string{});
            if (window.app_id.empty()) {
                window.app_id = client.value("initialClass", std::string{});
            }
            if (window.app_id.empty()) continue;

            window.title = client.value("title", std::string{});
            if (window.title.empty()) {
                window.title = client.value("initialTitle", std::string{});
            }
            if (window.title.empty()) window.title = "Untitled window";

            if (client.contains("workspace") && client["workspace"].is_object()) {
                const auto& workspace = client["workspace"];
                if (workspace.contains("id") && workspace["id"].is_number_integer()) {
                    window.workspace_id = workspace["id"].get<int>();
                }
            }
            if (client.contains("focusHistoryID") &&
                client["focusHistoryID"].is_number_integer()) {
                window.focus_history_id = client["focusHistoryID"].get<int>();
            }
            window.active = !active_address.empty() && window.address == active_address;
            snapshot.windows.push_back(std::move(window));
        }

        std::stable_sort(
            snapshot.windows.begin(),
            snapshot.windows.end(),
            [](const auto& left, const auto& right) {
                if (left.active != right.active) return left.active;
                if (left.focus_history_id != right.focus_history_id) {
                    return left.focus_history_id < right.focus_history_id;
                }
                return left.address < right.address;
            }
        );
        return snapshot;
    } catch (const json::exception&) {
        return unavailable("unable to parse Hyprland client JSON");
    }
}

bool HyprlandSession::focus_window(
    std::string_view address,
    const realmheart::core::CommandOptions& options
) {
    if (!realmheart::core::command_exists("hyprctl")) return false;
    const std::string normalized = normalize_address(address);
    if (normalized.empty()) return false;

    const std::string expression =
        "hl.dsp.focus({ window = \"address:" + normalized + "\" })";
    const auto result = realmheart::core::run_capture(
        {"hyprctl", "dispatch", expression},
        options
    );
    if (result.succeeded() && result.output.find("error:") == std::string::npos) {
        return true;
    }

    // Keep compatibility with Hyprland versions that have not adopted the
    // expression dispatcher yet.
    const auto fallback = realmheart::core::run_capture(
        {"hyprctl", "dispatch", "focuswindow", "address:" + normalized},
        options
    );
    return fallback.succeeded() && fallback.output.find("error:") == std::string::npos;
}

bool HyprlandSession::move_window_to_workspace(
    std::string_view address,
    int workspace_id,
    const realmheart::core::CommandOptions& options
) {
    const std::string normalized = normalize_address(address);
    if (workspace_id <= 0 || normalized.empty()) return false;
    if (!realmheart::core::command_exists("hyprctl")) return false;

    const std::string expression =
        "hl.dsp.window.move({ workspace = " + std::to_string(workspace_id) +
        ", follow = false, window = \"address:" + normalized + "\" })";
    const auto result = realmheart::core::run_capture(
        {"hyprctl", "dispatch", expression},
        options
    );
    if (result.succeeded() && result.output.find("error:") == std::string::npos) {
        return true;
    }

    // Hyprland 0.54 and older use the legacy named dispatcher. Keep the
    // fallback so Realmheart remains compatible across both dispatcher APIs.
    const std::string legacy_target = std::to_string(workspace_id) +
        ",address:" + normalized;
    const auto fallback = realmheart::core::run_capture(
        {"hyprctl", "dispatch", "movetoworkspacesilent", legacy_target},
        options
    );
    return fallback.succeeded() &&
        fallback.output.find("error:") == std::string::npos;
}

} // namespace realmheart::services
