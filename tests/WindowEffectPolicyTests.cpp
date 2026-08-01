#include "WindowEffectPolicy.hpp"

#include <cassert>
#include <string_view>

namespace {

void expectPools(
    const SWindowEffectConfig& config,
    std::string_view windowClass,
    std::string_view windowTitle,
    const WindowEffectPool& open,
    const WindowEffectPool& close
) {
    assert(!automaticWindowClassIsExcluded(windowClass));
    assert(automaticOpenEffectsForWindow(config, windowClass, windowTitle) == open);
    assert(automaticCloseEffectsForWindow(config, windowClass, windowTitle) == close);
}

void expectExcluded(
    const SWindowEffectConfig& config,
    std::string_view windowClass
) {
    assert(automaticWindowClassIsExcluded(windowClass));
    assert(
        automaticOpenEffectsForWindow(config, windowClass, "title") ==
        WindowEffectPool({"none"})
    );
    assert(
        automaticCloseEffectsForWindow(config, windowClass, "title") ==
        WindowEffectPool({"none"})
    );
}

} // namespace

int main() {
    const auto registry = loadWindowEffectRegistry(REALMHEART_TEST_EFFECT_DIR);
    assert(registry.success);

    const auto builtIn = builtInWindowEffectConfig();
    expectPools(
        builtIn,
        "kitty",
        "fish",
        builtIn.defaultOpenEffects,
        builtIn.defaultCloseEffects
    );
    expectPools(
        builtIn,
        "org.kde.dolphin",
        "Downloads — Dolphin",
        builtIn.defaultOpenEffects,
        builtIn.defaultCloseEffects
    );

    SWindowEffectConfig custom;
    custom.defaultOpenEffects = {"aether-sunder", "void"};
    custom.defaultCloseEffects = {"void", "aether-sunder"};
    custom.rules = {
        {
            .windowClass = std::string{"org.kde."},
            .classMatch = EWindowEffectTextMatch::Prefix,
            .windowTitle = std::nullopt,
            .titleMatch = EWindowEffectTextMatch::Contains,
            .openEffects = WindowEffectPool{"void"},
            .closeEffects = WindowEffectPool{"none", "aether-sunder"},
        },
        {
            .windowClass = std::nullopt,
            .classMatch = EWindowEffectTextMatch::Exact,
            .windowTitle = std::string{"Picture-in-Picture"},
            .titleMatch = EWindowEffectTextMatch::Contains,
            .openEffects = WindowEffectPool{"none"},
            .closeEffects = WindowEffectPool{"none"},
        },
    };

    expectPools(
        custom,
        "org.kde.dolphin",
        "Downloads — Dolphin",
        WindowEffectPool{"void"},
        WindowEffectPool{"none", "aether-sunder"}
    );
    expectPools(
        custom,
        "firefox",
        "Picture-in-Picture",
        WindowEffectPool{"none"},
        WindowEffectPool{"none"}
    );
    expectPools(
        custom,
        "firefox",
        "Realmheart docs",
        WindowEffectPool({"aether-sunder", "void"}),
        WindowEffectPool({"void", "aether-sunder"})
    );

    const WindowEffectPool randomPool{"void", "aether-sunder", "none"};
    assert(chooseWindowEffect(randomPool, 0U) == "void");
    assert(chooseWindowEffect(randomPool, 1U) == "aether-sunder");
    assert(chooseWindowEffect(randomPool, 2U) == "none");
    assert(chooseWindowEffect(randomPool, 3U) == "void");
    assert(chooseWindowEffect({}, 99U) == "none");

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
