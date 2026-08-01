#include "WindowEffectPolicy.hpp"

#include <cassert>
#include <string_view>

namespace {

void expectVoid(std::string_view windowClass) {
    assert(!automaticWindowClassIsExcluded(windowClass));
    assert(automaticOpenEffectForWindowClass(windowClass) == EWindowEffectId::Void);
    assert(automaticCloseEffectForWindowClass(windowClass) == EWindowEffectId::Void);
}

void expectAetherSunder(std::string_view windowClass) {
    assert(!automaticWindowClassIsExcluded(windowClass));
    assert(
        automaticOpenEffectForWindowClass(windowClass) ==
        EWindowEffectId::AetherSunder
    );
    assert(
        automaticCloseEffectForWindowClass(windowClass) ==
        EWindowEffectId::AetherSunder
    );
}

void expectExcluded(std::string_view windowClass) {
    assert(automaticWindowClassIsExcluded(windowClass));
    assert(automaticOpenEffectForWindowClass(windowClass) == EWindowEffectId::None);
    assert(automaticCloseEffectForWindowClass(windowClass) == EWindowEffectId::None);
}

} // namespace

int main() {
    expectAetherSunder("kitty");
    expectAetherSunder("Kitty");
    expectVoid("org.kde.dolphin");
    expectVoid("org.kde.kate");
    expectVoid("ORG.KDE.KONSOLE");
    expectVoid("firefox");
    expectVoid("code");
    expectVoid("discord");
    expectVoid("com.github.wwmm.easyeffects");

    expectExcluded("");
    expectExcluded("realmheart");
    expectExcluded("Realmheart-Launcher");
    expectExcluded("hyprlock");
    expectExcluded("Hyprlock-Surface");
    expectExcluded("gamescope");
    expectExcluded("gamescope-wl");
    expectExcluded("steam_app_123456");
    expectExcluded("STEAM_APP_987654");
    expectExcluded("xdg-desktop-portal-gtk");
    expectExcluded("org.freedesktop.impl.portal.desktop.kde");
    expectExcluded("xwaylandvideobridge");
    expectExcluded("org.kde.xwaylandvideobridge");
    expectExcluded("org.kde.polkit-kde-authentication-agent-1");
    expectExcluded("polkit-gnome-authentication-agent-1");
    expectExcluded("pinentry-qt");
    expectExcluded("ksshaskpass");
    expectExcluded("gcr-prompter");

    return 0;
}
