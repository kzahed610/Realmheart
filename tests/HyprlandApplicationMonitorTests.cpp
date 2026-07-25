#include <gtest/gtest.h>

#include "services/HyprlandApplicationMonitor.hpp"

using namespace realmheart::services;

TEST(HyprlandApplicationMonitorTest, ParsesFocusedApplicationClass) {
    const auto event = parse_hyprland_application_event(
        "activewindow>>zen,Realmheart launcher design"
    );

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, HyprlandApplicationEventKind::Focused);
    EXPECT_EQ(event->app_identity, "zen");
}

TEST(HyprlandApplicationMonitorTest, ParsesOpenedApplicationClass) {
    const auto event = parse_hyprland_application_event(
        "openwindow>>55dd22,2,org.kde.dolphin,Downloads"
    );

    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->kind, HyprlandApplicationEventKind::Opened);
    EXPECT_EQ(event->app_identity, "org.kde.dolphin");
}

TEST(HyprlandApplicationMonitorTest, ReportsSessionContextChanges) {
    const auto close = parse_hyprland_application_event("closewindow>>55dd22");
    ASSERT_TRUE(close.has_value());
    EXPECT_EQ(close->kind, HyprlandApplicationEventKind::ContextChanged);

    const auto title = parse_hyprland_application_event(
        "windowtitlev2>>55dd22,Updated title"
    );
    ASSERT_TRUE(title.has_value());
    EXPECT_EQ(title->kind, HyprlandApplicationEventKind::ContextChanged);
}

TEST(HyprlandApplicationMonitorTest, IgnoresEventsWithoutUsefulContext) {
    EXPECT_FALSE(parse_hyprland_application_event("activewindow>>,Desktop").has_value());
    EXPECT_FALSE(parse_hyprland_application_event("workspace>>3").has_value());
}
