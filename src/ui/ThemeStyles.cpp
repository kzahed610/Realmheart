#include "ui/ThemeStyles.hpp"

#include <stdexcept>

namespace realmheart::ui {

ThemeStyles::ThemeStyles(std::shared_ptr<services::ThemeService> theme_service)
    : theme_service_(std::move(theme_service)),
      display_(gdk_display_get_default()),
      provider_(gtk_css_provider_new()) {
    if (!theme_service_) throw std::invalid_argument("ThemeStyles requires ThemeService");

    if (display_ != nullptr) {
        gtk_style_context_add_provider_for_display(
            display_,
            GTK_STYLE_PROVIDER(provider_),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
    }

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
    const std::string css = build_css(palette);
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
        "\n"
        ".realmheart-right-sidebar {\n"
        "  background-color: alpha(@rh_background, 0.94);\n"
        "  color: @rh_text;\n"
        "  border-left: 1px solid alpha(@rh_outline, 0.65);\n"
        "}\n"
        ".realmheart-sidebar-header {\n"
        "  color: @rh_text;\n"
        "  font-weight: 700;\n"
        "  font-size: 1.08em;\n"
        "}\n"
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
        ".realmheart-notification-row {\n"
        "  background-color: alpha(@rh_surface, 0.72);\n"
        "  border-radius: 10px;\n"
        "  padding: 7px;\n"
        "}\n"
        ".realmheart-notification-body { color: @rh_text_muted; }\n"
        "\n"
        ".realmheart-vertical-bar-window {\n"
        "  background-color: alpha(@rh_background, 0.92);\n"
        "  border-right: 1px solid alpha(@rh_outline, 0.55);\n"
        "}\n"
        ".realmheart-vertical-bar {\n"
        "  background-color: transparent;\n"
        "  color: @rh_text;\n"
        "  padding: 4px;\n"
        "}\n"
        ".realmheart-bar-clock { color: @rh_text; font-weight: 700; }\n"
        ".realmheart-bar-pill {\n"
        "  min-width: 30px;\n"
        "  min-height: 30px;\n"
        "  padding: 2px;\n"
        "  color: @rh_text_muted;\n"
        "  background-color: alpha(@rh_surface, 0.78);\n"
        "  border: 1px solid alpha(@rh_outline, 0.35);\n"
        "  border-radius: 999px;\n"
        "}\n"
        ".realmheart-bar-pill-active {\n"
        "  color: @rh_text;\n"
        "  background-color: alpha(@rh_primary, 0.30);\n"
        "  border-color: @rh_primary;\n"
        "}\n"
        ".realmheart-bar-pill-occupied { color: @rh_secondary; }\n"
        ".realmheart-bar-status {\n"
        "  min-width: 34px;\n"
        "  min-height: 34px;\n"
        "  padding: 4px;\n"
        "  color: @rh_text;\n"
        "  background-color: alpha(@rh_surface, 0.78);\n"
        "  border: 1px solid alpha(@rh_outline, 0.35);\n"
        "  border-radius: 12px;\n"
        "}\n"
        ".realmheart-bar-status-enabled { border-color: alpha(@rh_primary, 0.88); }\n"
        ".realmheart-bar-status-disabled { color: @rh_text_muted; opacity: 0.75; }\n"
        ".realmheart-bar-badge {\n"
        "  color: @rh_background;\n"
        "  background-color: @rh_error;\n"
        "  border-radius: 999px;\n"
        "  padding: 1px 4px;\n"
        "  font-size: 0.72em;\n"
        "  font-weight: 700;\n"
        "}\n"
        "\n"
        ".realmheart-notes {\n"
        "  background-color: alpha(@rh_background, 0.94);\n"
        "  color: @rh_text;\n"
        "  border: 2px solid @rh_primary;\n"
        "  border-radius: 12px;\n"
        "}\n"
        ".realmheart-notes-editor, .realmheart-notes-editor text {\n"
        "  background-color: transparent;\n"
        "  color: @rh_text;\n"
        "  font-family: 'JetBrains Mono';\n"
        "  font-size: 14px;\n"
        "}\n";
}

} // namespace realmheart::ui
