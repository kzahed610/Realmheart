#include "WindowEffectPolicy.hpp"

#include <cassert>
#include <string_view>

namespace {

void expectEffects(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle,
    EWindowEffectId open,
    EWindowEffectId close
) {
    assert(!automaticWindowClassIsExcluded(windowClass));
    assert(automaticOpenEffectForWindow(config, windowClass, windowTitle) == open);
    assert(automaticCloseEffectForWindow(config, windowClass, windowTitle) == close);
}

void expectExcluded(
    const SWindowEffectConfig& config,
    std::string_view windowClass
) {
    assert(automaticWindowClassIsExcluded(windowClass));
    assert(
        automaticOpenEffectForWindow(config, windowClass, "title") ==
        EWindowEffectId::None
    );
    assert(
        automaticCloseEffectForWindow(config, windowClass, "title") ==
        EWindowEffectId::None
    );
}

} // namespace

int main() {
    const auto builtIn = builtInWindowEffectConfig();
    expectEffects(
        builtIn,
        "kitty",
        "fish",
        EWindowEffectId::AetherSunder,
        EWindowEffectId::AetherSunder
    );
    expectEffects(
        builtIn,
        "Kitty",
        "fish",
        EWindowEffectId::AetherSunder,
        EWindowEffectId::AetherSunder
    );
    expectEffects(
        builtIn,
        "org.kde.dolphin",
        "Downloads — Dolphin",
        EWindowEffectId::Void,
        EWindowEffectId::Void
    );
    expectEffects(
        builtIn,
        "org.kde.kate",
        "notes.txt — Kate",
        EWindowEffectId::Void,
        EWindowEffectId::Void
    );

    SWindowEffectConfig custom;
    custom.defaultOpenEffect = EWindowEffectId::AetherSunder;
    custom.defaultCloseEffect = EWindowEffectId::Void;
    custom.rules = {
        {
            .windowClass = std::string{"org.kde."},
            .classMatch = EWindowEffectTextMatch::Prefix,
            .windowTitle = std::nullopt,
            .titleMatch = EWindowEffectTextMatch::Contains,
            .openEffect = EWindowEffectId::Void,
            .closeEffect = EWindowEffectId::None,
        },
        {
            .windowClass = std::nullopt,
            .classMatch = EWindowEffectTextMatch::Exact,
            .windowTitle = std::string{"Picture-in-Picture"},
            .titleMatch = EWindowEffectTextMatch::Contains,
            .openEffect = EWindowEffectId::None,
            .closeEffect = EWindowEffectId::None,
        },
    };

    expectEffects(
        custom,
        "org.kde.dolphin",
        "Downloads — Dolphin",
        EWindowEffectId::Void,
        EWindowEffectId::None
    );
    expectEffects(
        custom,
        "firefox",
        "Picture-in-Picture",
        EWindowEffectId::None,
        EWindowEffectId::None
    );
    expectEffects(
        custom,
        "firefox",
        "Realmheart docs",
        EWindowEffectId::AetherSunder,
        EWindowEffectId::Void
    );

    expectExcluded(builtIn, "");
    expectExcluded(builtIn, "realmheart");
    expectExcluded(builtIn, "Realmheart-Launcher");
    expectExcluded(builtIn, "hyprlock");
    expectExcluded(builtIn, "Hyprlock-Surface");
    expectExcluded(builtIn, "gamescope");
    expectExcluded(builtIn, "gamescope-wl");
    expectExcluded(builtIn, "steam_app_123456");
    expectExcluded(builtIn, "STEAM_APP_987654");
    expectExcluded(builtIn, "xdg-desktop-portal-gtk");
    expectExcluded(builtIn, "org.freedesktop.impl.portal.desktop.kde");
    expectExcluded(builtIn, "xwaylandvideobridge");
    expectExcluded(builtIn, "org.kde.xwaylandvideobridge");
    expectExcluded(builtIn, "org.kde.polkit-kde-authentication-agent-1");
    expectExcluded(builtIn, "polkit-gnome-authentication-agent-1");
    expectExcluded(builtIn, "pinentry-qt");
    expectExcluded(builtIn, "ksshaskpass");
    expectExcluded(builtIn, "gcr-prompter");

    return 0;
}
