#include "WindowEffectPolicy.hpp"

#include <cassert>
#include <string_view>

namespace {

void expectEffects(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle,
    std::string_view open,
    std::string_view close
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
        std::string{"none"}
    );
    assert(
        automaticCloseEffectForWindow(config, windowClass, "title") ==
        std::string{"none"}
    );
}

} // namespace

int main() {
    const auto registry = loadWindowEffectRegistry(REALMHEART_TEST_EFFECT_DIR);
    assert(registry.success);

    const auto builtIn = builtInWindowEffectConfig();
    expectEffects(
        builtIn,
        "kitty",
        "fish",
        std::string{"aether-sunder"},
        std::string{"aether-sunder"}
    );
    expectEffects(
        builtIn,
        "Kitty",
        "fish",
        std::string{"aether-sunder"},
        std::string{"aether-sunder"}
    );
    expectEffects(
        builtIn,
        "org.kde.dolphin",
        "Downloads — Dolphin",
        std::string{"void"},
        std::string{"void"}
    );
    expectEffects(
        builtIn,
        "org.kde.kate",
        "notes.txt — Kate",
        std::string{"void"},
        std::string{"void"}
    );

    SWindowEffectConfig custom;
    custom.defaultOpenEffect = std::string{"aether-sunder"};
    custom.defaultCloseEffect = std::string{"void"};
    custom.rules = {
        {
            .windowClass = std::string{"org.kde."},
            .classMatch = EWindowEffectTextMatch::Prefix,
            .windowTitle = std::nullopt,
            .titleMatch = EWindowEffectTextMatch::Contains,
            .openEffect = std::string{"void"},
            .closeEffect = std::string{"none"},
        },
        {
            .windowClass = std::nullopt,
            .classMatch = EWindowEffectTextMatch::Exact,
            .windowTitle = std::string{"Picture-in-Picture"},
            .titleMatch = EWindowEffectTextMatch::Contains,
            .openEffect = std::string{"none"},
            .closeEffect = std::string{"none"},
        },
    };

    expectEffects(
        custom,
        "org.kde.dolphin",
        "Downloads — Dolphin",
        std::string{"void"},
        std::string{"none"}
    );
    expectEffects(
        custom,
        "firefox",
        "Picture-in-Picture",
        std::string{"none"},
        std::string{"none"}
    );
    expectEffects(
        custom,
        "firefox",
        "Realmheart docs",
        std::string{"aether-sunder"},
        std::string{"void"}
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
