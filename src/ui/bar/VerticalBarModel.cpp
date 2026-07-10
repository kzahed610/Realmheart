#include "ui/bar/VerticalBarModel.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>

namespace realmheart::ui::bar {
namespace {

struct SlotSpec {
    std::string_view name;
    std::string_view icon_name;
    std::string_view fallback_text;
};

constexpr std::array<SlotSpec, 8> kServiceSlots{{
    {"WiFi", "wifi-4.svg", "Wi"},
    {"Bluetooth", "bluetooth.svg", "Bt"},
    {"Keep Awake", "shield.svg", "Aw"},
    {"Night Light", "weather-moon.svg", "NL"},
    {"Gamemode", "games.svg", "Gm"},
    {"Power Profile", "battery-saver.svg", "Pw"},
    {"Brightness", "weather-sunny.svg", "Br"},
    {"Volume", "speaker-2-filled.svg", "Vo"},
}};

} // namespace

std::vector<realmheart::services::WorkspaceState> build_workspace_pills(
    const realmheart::services::WorkspaceSnapshot& snapshot
) {
    std::vector<realmheart::services::WorkspaceState> workspaces;
    std::set<int> included;

    for (int id = 1; id <= 5; ++id) {
        workspaces.push_back({id, std::to_string(id), 0, snapshot.available && id == snapshot.active_id});
        included.insert(id);
    }

    if (!snapshot.available) return workspaces;

    for (const auto& workspace : snapshot.workspaces) {
        if (workspace.id <= 0 || workspace.id > 10) continue;

        auto existing = std::find_if(workspaces.begin(), workspaces.end(), [&workspace](const auto& item) {
            return item.id == workspace.id;
        });
        if (existing != workspaces.end()) {
            *existing = workspace;
        } else if (!included.contains(workspace.id)) {
            workspaces.push_back(workspace);
            included.insert(workspace.id);
        }
    }

    std::sort(workspaces.begin(), workspaces.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return workspaces;
}

std::vector<BarStatusSlot> build_status_slots(
    const std::vector<realmheart::services::ServiceStatus>& report,
    const realmheart::services::NotificationSnapshot& notifications
) {
    std::vector<BarStatusSlot> slots;
    slots.reserve(kServiceSlots.size() + 1);

    for (const auto& spec : kServiceSlots) {
        const auto status = std::find_if(report.begin(), report.end(), [&spec](const auto& candidate) {
            return candidate.name == spec.name;
        });

        BarStatusSlot slot;
        slot.name = spec.name;
        slot.icon_name = spec.icon_name;
        slot.fallback_text = spec.fallback_text;
        slot.enabled = status != report.end() && status->enabled;
        slot.tooltip = slot.name + ": " + (status == report.end() ? "status pending" : status->status);
        slots.push_back(std::move(slot));
    }

    BarStatusSlot notifications_slot;
    notifications_slot.name = "Notifications";
    notifications_slot.icon_name = "alert.svg";
    notifications_slot.fallback_text = "Nt";
    if (notifications.capture_active) {
        notifications_slot.enabled = notifications.unread_count > 0;
        notifications_slot.tooltip = "Notifications: " + std::to_string(notifications.unread_count) + " unread";
        if (notifications.unread_count > 0) {
            notifications_slot.badge_text = std::to_string(notifications.unread_count);
        }
    } else {
        notifications_slot.tooltip = "Notifications: capture pending";
    }
    slots.push_back(std::move(notifications_slot));

    return slots;
}

} // namespace realmheart::ui::bar
