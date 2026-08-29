#include "ui/ThemeStyles.hpp"

#include "ui/styles/CssModuleLoader.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace realmheart::ui {
namespace {

// Realmheart owns the complete visual treatment of its shell surfaces. Load
// its provider above ~/.config/gtk-4.0/gtk.css (USER priority), whose global
// `window { background: ... }` rule otherwise paints native popover corners.
constexpr guint kRealmheartStylePriority = GTK_STYLE_PROVIDER_PRIORITY_USER + 1;

std::optional<double> hex_luminance(std::string_view color) {
    if (color.size() != 7 || color.front() != '#') return std::nullopt;
    const auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    const auto channel = [&](std::size_t index) -> std::optional<double> {
        const int high = hex(color[index]);
        const int low = hex(color[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        const double srgb = static_cast<double>((high << 4) | low) / 255.0;
        return srgb <= 0.04045
            ? srgb / 12.92
            : std::pow((srgb + 0.055) / 1.055, 2.4);
    };

    const auto red = channel(1);
    const auto green = channel(3);
    const auto blue = channel(5);
    if (!red || !green || !blue) return std::nullopt;
    return (0.2126 * *red) + (0.7152 * *green) + (0.0722 * *blue);
}

bool palette_is_dark(std::string_view background) {
    return hex_luminance(background).value_or(0.0) < 0.42;
}

} // namespace

ThemeStyles::ThemeStyles(std::shared_ptr<services::ThemeService> theme_service)
    : theme_service_(std::move(theme_service)),
      display_(gdk_display_get_default()),
      provider_(gtk_css_provider_new()) {
    if (!theme_service_) throw std::invalid_argument("ThemeStyles requires ThemeService");

    if (display_ != nullptr) {
        gtk_style_context_add_provider_for_display(
            display_,
            GTK_STYLE_PROVIDER(provider_),
            kRealmheartStylePriority
        );
    }

    constexpr std::array<std::string_view, 7> component_modules{
        "taskbar/bar.css",
        "taskbar/icons.css",
        "taskbar/workspaces.css",
        "taskbar/popovers.css",
        "launcher/launcher.css",
        "launcher/command-receipt.css",
        "lockscreen/lockscreen.css",
    };
    component_css_ = styles::load_css_modules(component_modules);

    // Hardcode ManaCores selector transparency rules globally to avoid adding
    // providers dynamically at runtime, which triggers a global style update that
    // can crash GTK if executed while other widgets are handling input or transitions.
    component_css_ += R"CSS(
        .realmheart-mana-cores-window,
        .realmheart-mana-cores-window * {
            background: transparent;
            background-color: transparent;
            border: none;
            box-shadow: none;
        }
        .realmheart-mana-cores-canvas {
            background: transparent;
            background-color: transparent;
        }
        window.realmheart-vertical-bar-window,
        window.realmheart-vertical-bar-window > * {
            background: transparent;
            background-color: transparent;
            background-image: none;
            box-shadow: none;
        }
        /* Bar buttons/rune buttons are nested inside layout boxes (not direct
           children of the window), so the rule above misses them and Adwaita
           paints a white background on fresh accounts that lack a dark gtk.css.
           Target the specific button classes so separators/layout boxes keep
           their borders, and active-state classes still win (they attach a
           second class for higher specificity). */
        window.realmheart-vertical-bar-window .realmheart-bar-icon-button,
        window.realmheart-vertical-bar-window .realmheart-workspace-rune {
            background: transparent;
            background-color: transparent;
            background-image: none;
            border: none;
            box-shadow: none;
        }
        /* GTK default Adwaita is light-themed; force the colour scheme so the
           bar/buttons render against the user's wallpaper palette even when
           no Matugen gtk.css has been generated for this account yet. */
        :root {
            color-scheme: dark;
        }
    )CSS";

    subscription_ = theme_service_->subscribe([this](const services::Palette& palette) {
        apply(palette);
    });
    apply(theme_service_->get_palette());
}

ThemeStyles::~ThemeStyles() {
    subscription_.reset();
    if (display_ != nullptr && provider_ != nullptr) {
        gtk_style_context_remove_provider_for_display(
            display_,
            GTK_STYLE_PROVIDER(provider_)
        );
    }
    if (provider_ != nullptr) {
        g_object_unref(provider_);
        provider_ = nullptr;
    }
}

void ThemeStyles::apply(const services::Palette& palette) {
    std::string css = build_css(palette);
    css += component_css_;
    gtk_css_provider_load_from_string(provider_, css.c_str());
}

std::string ThemeStyles::build_css(const services::Palette& palette) {
    const auto primary = palette.get("primary", "#cba6f7");
    const auto secondary = palette.get("secondary", "#89b4fa");
    const auto background = palette.get("background", "#11111b");
    const auto surface = palette.get("surface", "#1e1e2e");
    const auto surface_variant = palette.get("surface_variant", "#313244");
    const auto text = palette.get("text", "#cdd6f4");
    const auto text_muted = palette.get("text_muted", "#a6adc8");
    const auto outline = palette.get("outline", "#45475a");
    const auto error = palette.get("error", "#f38ba8");
    const bool dark = palette_is_dark(background);
    const std::string icon_primary = dark ? "#F5F2EA" : "#17141D";
    const std::string icon_accent = dark ? "#FFD66B" : "#6D42D8";
    const std::string icon_on_accent = dark ? "#18151F" : "#FFFFFF";

    return
        "@define-color rh_primary " + primary + ";\n"
        "@define-color rh_secondary " + secondary + ";\n"
        "@define-color rh_background " + background + ";\n"
        "@define-color rh_surface " + surface + ";\n"
        "@define-color rh_surface_variant " + surface_variant + ";\n"
        "@define-color rh_text " + text + ";\n"
        "@define-color rh_text_muted " + text_muted + ";\n"
        "@define-color rh_outline " + outline + ";\n"
        "@define-color rh_error " + error + ";\n"
        "@define-color rh_icon_primary " + icon_primary + ";\n"
        "@define-color rh_icon_accent " + icon_accent + ";\n"
        "@define-color rh_icon_on_accent " + icon_on_accent + ";\n"
        "\n"
        "/* Every Realmheart layer surface must be structurally transparent. The\n"
        "   host's matugen gtk.css paints `window { background: <scheme-bg> }` on\n"
        "   all windows except a hand-picked :not() list; on a light-scheme\n"
        "   fallback that is a near-white #fcf8ff. These rules (higher provider\n"
        "   priority than USER) guarantee no Realmheart window ever inherits it.\n"
        "   Keep this list in sync with every window class added to the shell. */\n"
        "window.realmheart-vertical-bar-window,\n"
        "window.realmheart-sidebar-window,\n"
        "window.realmheart-right-hotspot-window,\n"
        "window.realmheart-sidebar-backdrop-window,\n"
        "window.realmheart-launcher-window,\n"
        "window.realmheart-notification-window,\n"
        "window.realmheart-now-playing-window,\n"
        "window.realmheart-osd-window,\n"
        "window.realmheart-media-layer-window,\n"
        "window.realmheart-system-monitor-layer-window,\n"
        "window.realmheart-workspace-overview-window,\n"
        "window.realmheart-workspace-preview-window,\n"
        "window.realmheart-power-menu-window,\n"
        "window.realmheart-mana-cores-window,\n"
        "window.realmheart-broken-seal-window {\n"
        "  background-color: transparent;\n"
        "  background-image: none;\n"
        "  box-shadow: none;\n"
        "}\n"
        "window.realmheart-power-menu-window,\n"
        "window.realmheart-power-menu-window > *,\n"
        ".realmheart-power-menu-root,\n"
        ".realmheart-power-menu-scene,\n"
        ".realmheart-power-menu-media,\n"
        ".realmheart-power-menu-poster,\n"
        ".realmheart-power-menu-video,\n"
        ".realmheart-power-menu-ripple {\n"
        "  background-color: rgba(0, 0, 0, 0);\n"
        "  background-image: none;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "}\n"
        "window.realmheart-sidebar-window,\n"
        "window.realmheart-sidebar-window > * {\n"
        "  background-color: transparent;\n"
        "  box-shadow: none;\n"
        "}\n"
        "window.realmheart-right-hotspot-window,\n"
        "window.realmheart-right-hotspot-window > *,\n"
        "window.realmheart-sidebar-backdrop-window,\n"
        "window.realmheart-sidebar-backdrop-window > * {\n"
        "  background-color: transparent;\n"
        "  background-image: none;\n"
        "  box-shadow: none;\n"
        "}\n"
        ".realmheart-right-hotspot-button,\n"
        ".realmheart-right-hotspot-button:hover,\n"
        ".realmheart-right-hotspot-button:active,\n"
        ".realmheart-right-hotspot-button:focus,\n"
        ".realmheart-right-hotspot-button:focus-visible {\n"
        "  min-width: 16px;\n"
        "  min-height: 0;\n"
        "  margin: 0;\n"
        "  padding: 0;\n"
        "  border: 0;\n"
        "  border-radius: 0;\n"
        "  outline: none;\n"
        "  background-color: transparent;\n"
        "  background-image: none;\n"
        "  box-shadow: none;\n"
        "  color: transparent;\n"
        "}\n"
        ".realmheart-sidebar-backdrop-button,\n"
        ".realmheart-sidebar-backdrop-button:hover,\n"
        ".realmheart-sidebar-backdrop-button:active,\n"
        ".realmheart-sidebar-backdrop-button:focus,\n"
        ".realmheart-sidebar-backdrop-button:focus-visible {\n"
        "  min-width: 0;\n"
        "  min-height: 0;\n"
        "  margin: 0;\n"
        "  padding: 0;\n"
        "  border: 0;\n"
        "  border-radius: 0;\n"
        "  outline: none;\n"
        "  background-color: transparent;\n"
        "  background-image: none;\n"
        "  box-shadow: none;\n"
        "  color: transparent;\n"
        "}\n"
        ".realmheart-sidebar-frame { background-color: transparent; }\n"
        ".realmheart-sidebar-frame-shadow { color: #000000; }\n"
        ".realmheart-sidebar-back-art,\n"
        ".realmheart-sidebar-front-art,\n"
        ".realmheart-sidebar-content-layer { background-color: transparent; }\n"
        ".realmheart-sidebar-frame-outer-fill {\n"
        "  color: alpha(#9a6829, 0.82);\n"
        "}\n"
        ".realmheart-sidebar-frame-middle-fill {\n"
        "  color: alpha(#211b14, 0.99);\n"
        "}\n"
        ".realmheart-sidebar-frame-inner-fill {\n"
        "  color: alpha(@rh_background, 0.982);\n"
        "}\n"
        ".realmheart-sidebar-frame-outer-contour { color: alpha(#d8b568, 0.90); }\n"
        ".realmheart-sidebar-frame-inner-contour { color: alpha(#765126, 0.75); }\n"
        ".realmheart-sidebar-frame-highlight-contour {\n"
        "  color: alpha(#f6dda2, 0.38);\n"
        "}\n"
        ".realmheart-sidebar-frame-gold-detail { color: alpha(#d7aa51, 0.72); }\n"
        ".realmheart-sidebar-frame-violet-detail { color: alpha(@rh_primary, 0.94); }\n"
        ".realmheart-right-sidebar {\n"
        "  background-color: transparent;\n"
        "  color: @rh_text;\n"
        "}\n"
        ".realmheart-sidebar-header {\n"
        "  color: @rh_text;\n"
        "  font-weight: 700;\n"
        "  font-size: 1.08em;\n"
        "}\n"
        ".realmheart-sidebar-scroller { background-color: transparent; }\n"
        ".realmheart-module-row {\n"
        "  color: @rh_text;\n"
        "  background-color: alpha(@rh_surface, 0.72);\n"
        "  border: 1px solid alpha(@rh_outline, 0.35);\n"
        "  border-radius: 12px;\n"
        "  padding: 7px 9px;\n"
        "}\n"
        ".realmheart-module-value { color: @rh_primary; font-weight: 700; }\n"
        ".realmheart-module-button { color: @rh_text; }\n"
        ".realmheart-module-button button {\n"
        "  color: @rh_text;\n"
        "  background-color: alpha(@rh_surface_variant, 0.86);\n"
        "  border-color: alpha(@rh_outline, 0.55);\n"
        "}\n"
        ".realmheart-module-slider { color: @rh_text; }\n"
        ".realmheart-module-slider highlight { background-color: @rh_primary; }\n"
        ".realmheart-notifications { color: @rh_text; }\n"
        ".realmheart-notification-row { color: @rh_text; }\n"
        ".realmheart-notification-body { color: @rh_text_muted; }\n"
        "\n"
        "\n"
        ".realmheart-notes {\n"
        "  background-color: alpha(@rh_background, 0.96);\n"
        "  color: @rh_text;\n"
        "  border: 1px solid alpha(@rh_primary, 0.45);\n"
        "  border-radius: 12px;\n"
        "}\n"
        ".realmheart-notes-header {\n"
        "  padding: 14px 18px 10px 18px;\n"
        "  border-bottom: 1px solid alpha(@rh_primary, 0.22);\n"
        "}\n"
        ".realmheart-notes-title {\n"
        "  color: alpha(#e7c27a, 0.92);\n"
        "  font-family: Cinzel, serif;\n"
        "  font-size: 0.72em;\n"
        "  font-weight: 700;\n"
        "  letter-spacing: 2.2px;\n"
        "}\n"
        ".realmheart-notes-editor, .realmheart-notes-editor text {\n"
        "  background-color: transparent;\n"
        "  color: @rh_text;\n"
        "  font-family: 'JetBrains Mono';\n"
        "  font-size: 14px;\n"
        "  caret-color: @rh_primary;\n"
        "}\n"
        ".realmheart-notes-editor text > selection {\n"
        "  background-color: alpha(@rh_primary, 0.32);\n"
        "}\n"
        ".realmheart-notes-save-state {\n"
        "  color: @rh_text_muted;\n"
        "  font-size: 11px;\n"
        "  font-weight: 600;\n"
        "}\n"
        ".realmheart-notes-save-state.pending { color: @rh_secondary; }\n"
        ".realmheart-notes-save-state.failed { color: @rh_error; font-weight: 700;\n"
        "}\n";
}

} // namespace realmheart::ui
