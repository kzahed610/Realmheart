#include "ui/bar/VerticalBarModel.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_workspace_pills_keep_defaults_and_merge_live_state() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = 3;
    snapshot.workspaces = {
        {3, "dev", 2, true},
        {7, "seven", 1, false},
    };

    const auto pills = realmheart::ui::bar::build_workspace_pills(snapshot);
    require(pills.size() == 6, "bar must retain workspaces 1-5 and append valid live workspaces");
    require(pills[2].id == 3 && pills[2].active && pills[2].windows == 2,
            "live workspace state must replace its default pill");
    require(pills.back().id == 7 && pills.back().name == "seven",
            "additional live workspaces must be appended in sorted order");
}

void test_status_slots_only_keep_compact_bar_information() {
    const std::vector<realmheart::services::ServiceStatus> report{
        {"WiFi", "Enabled (home)", true},
        {"Battery", "84% (Discharging)", true},
        {"Media", "Song - Artist", true},
    };
    realmheart::services::NotificationSnapshot notifications;

    const auto slots = realmheart::ui::bar::build_status_slots(report, notifications);
    require(slots.size() == 3, "bar must contain only battery, media, and notifications");
    require(slots[0].name == "Battery" && slots[0].tooltip == "Battery: 84% (Discharging)",
            "battery must remain visible on the compact bar");
    require(slots[1].name == "Media" && slots[1].tooltip == "Media: Song - Artist",
            "media must remain visible on the compact bar");
    require(slots[2].name == "Notifications", "notifications must remain visible on the compact bar");
}

void test_notification_slot_reports_capture_and_unread_state() {
    realmheart::services::NotificationSnapshot notifications;
    notifications.capture_active = true;
    notifications.unread_count = 12;

    const auto slots = realmheart::ui::bar::build_status_slots({}, notifications);
    const auto& notification_slot = slots.back();
    require(notification_slot.name == "Notifications", "notification slot must be last in the status cluster");
    require(notification_slot.enabled, "nonzero unread count must emphasize notifications");
    require(notification_slot.badge_text == "12", "notification badge must expose unread count");
    require(notification_slot.tooltip == "Notifications: 12 unread", "notification tooltip must expose unread count");

    notifications.capture_active = false;
    const auto inactive_slots = realmheart::ui::bar::build_status_slots({}, notifications);
    require(!inactive_slots.back().enabled, "inactive capture must not pretend notification state is live");
    require(inactive_slots.back().badge_text.empty(), "inactive capture must not show a misleading zero badge");
    require(inactive_slots.back().tooltip == "Notifications: capture pending",
            "coexistence state must be explicit until Phase 4.5 owns notification capture");
}

} // namespace

int main() {
    test_workspace_pills_keep_defaults_and_merge_live_state();
    test_status_slots_only_keep_compact_bar_information();
    test_notification_slot_reports_capture_and_unread_state();
    std::cout << "Vertical bar model tests passed\n";
    return 0;
}
