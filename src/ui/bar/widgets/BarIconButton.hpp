#pragma once

#include <functional>
#include <gtk/gtk.h>
#include <string>

namespace realmheart::ui::bar::widgets {

class BarIconButton {
public:
    BarIconButton(
        std::string asset_path,
        std::string fallback_text,
        std::string tooltip,
        std::function<void()> on_click = {}
    );
    ~BarIconButton();

    BarIconButton(const BarIconButton&) = delete;
    BarIconButton& operator=(const BarIconButton&) = delete;

    GtkWidget* widget() const { return button_; }
    GtkWidget* button() const { return button_; }

    void set_icon(std::string asset_path, std::string fallback_text);
    void set_tooltip(const std::string& tooltip);
    void set_enabled(bool enabled);
    void set_badge(const std::string& text);
    void add_css_class(const char* css_class);
    void set_icon_size(int pixels);

private:
    GtkWidget* button_ = nullptr;
    GtkWidget* overlay_ = nullptr;
    GtkWidget* stack_ = nullptr;
    GtkWidget* icon_ = nullptr;
    GtkWidget* fallback_ = nullptr;
    GtkWidget* badge_ = nullptr;
    gulong click_handler_ = 0;
    std::function<void()> on_click_;
};

} // namespace realmheart::ui::bar::widgets
