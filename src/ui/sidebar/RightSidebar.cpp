#include "ui/sidebar/RightSidebar.hpp"
#include "ui/LayerSurface.hpp"
#include "services/Brightness.hpp"
#include "services/PowerProfiles.hpp"

#include <gtk/gtk.h>
#include <iostream>

namespace realmheart::ui::sidebar {

// --- Concrete Modules ---

class LabelModule : public SidebarModule {
public:
    LabelModule(const std::string& label, const std::string& value) : SidebarModule(label) {
        box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        GtkWidget* lbl_name = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
        gtk_box_append(GTK_BOX(box_), lbl_name);

        val_label_ = gtk_label_new(value.c_str());
        gtk_label_set_xalign(GTK_LABEL(val_label_), 1.0);
        gtk_box_append(GTK_BOX(box_), val_label_);
    }

    GtkWidget* get_widget() override { return box_; }

    void set_value(const std::string& value) {
        gtk_label_set_text(GTK_LABEL(val_label_), value.c_str());
    }

private:
    GtkWidget* box_;
    GtkWidget* val_label_;
};

class ToggleModule : public SidebarModule {
public:
    ToggleModule(const std::string& label, std::function<void(bool)> on_toggle) 
        : SidebarModule(label), on_toggle_(on_toggle) {
        box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        GtkWidget* lbl_name = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
        gtk_box_append(GTK_BOX(box_), lbl_name);

        switch_ = gtk_switch_new();
        g_signal_connect(switch_, "state-set", G_CALLBACK(+[](GtkSwitch* w, gboolean state, gpointer data) {
            (void)w;
            auto* self = static_cast<ToggleModule*>(data);
            self->on_toggle_(state);
            return FALSE;
        }), this);
        gtk_box_append(GTK_BOX(box_), switch_);
    }

    GtkWidget* get_widget() override { return box_; }

private:
    GtkWidget* box_;
    GtkWidget* switch_;
    std::function<void(bool)> on_toggle_;
};

class SliderModule : public SidebarModule {
public:
    SliderModule(const std::string& label, double min, double max, double initial, std::function<void(double)> on_change)
        : SidebarModule(label), on_change_(on_change) {
        box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        GtkWidget* lbl_name = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
        gtk_box_append(GTK_BOX(box_), lbl_name);

        scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1.0);
        gtk_range_set_value(GTK_RANGE(scale_), initial);
        g_signal_connect(scale_, "value-changed", G_CALLBACK(+[](GtkRange* r, gpointer data) {
            auto* self = static_cast<SliderModule*>(data);
            self->on_change_(gtk_range_get_value(r));
        }), this);
        gtk_box_append(GTK_BOX(box_), scale_);
    }

    GtkWidget* get_widget() override { return box_; }

private:
    GtkWidget* box_;
    GtkWidget* scale_;
    std::function<void(double)> on_change_;
};

class ButtonModule : public SidebarModule {
public:
    ButtonModule(const std::string& label, std::function<void()> on_click) 
        : SidebarModule(label), on_click_(on_click) {
        box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        GtkWidget* lbl_name = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
        gtk_box_append(GTK_BOX(box_), lbl_name);

        btn_ = gtk_button_new_with_label("Cycle");
        g_signal_connect(btn_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ButtonModule*>(data);
            self->on_click_();
        }), this);
        gtk_box_append(GTK_BOX(box_), btn_);
    }

    GtkWidget* get_widget() override { return box_; }

private:
    GtkWidget* box_;
    GtkWidget* btn_;
    std::function<void()> on_click_;
};

// --- RightSidebar Implementation ---

RightSidebar::RightSidebar(GtkApplication* app) : app_(app) {
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Right Sidebar");
    gtk_window_set_default_size(GTK_WINDOW(window_), 300, 800);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    apply_layer_surface(GTK_WINDOW(window_), make_layer_surface_spec("realmheart-sidebar", LayerSurfaceLevel::Top, LayerKeyboardMode::Exclusive));
    
    setup_layout();
    populate_modules();
}

void RightSidebar::setup_layout() {
    container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(container_, 300, -1);
    
    GtkWidget* header = gtk_label_new("System Controls");
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 12);
    gtk_box_append(GTK_BOX(container_), header);

    gtk_window_set_child(GTK_WINDOW(window_), container_);
}

void RightSidebar::add_module(std::unique_ptr<SidebarModule> module) {
    modules_.push_back(std::move(module));
    gtk_box_append(GTK_BOX(container_), modules_.back()->get_widget());
}

void RightSidebar::populate_modules() {
    // 1. Simple Statuses
    add_module(std::make_unique<LabelModule>("WiFi", "Checking..."));
    add_module(std::make_unique<LabelModule>("Bluetooth", "Checking..."));
    
    // 2. Toggles
    add_module(std::make_unique<ToggleModule>("Gamemode", [](bool active) {
        std::cout << "Gamemode toggle: " << active << "\n";
    }));
    
    // 3. Power Profile
    add_module(std::make_unique<ButtonModule>("Power Profile", []() {
        if (auto next = services::PowerProfiles::cycle()) {
            std::cout << "Power profile cycled to: " << *next << "\n";
        }
    }));
    
    // 4. Brightness
    if (auto b = services::Brightness::read()) {
        add_module(std::make_unique<SliderModule>("Brightness", 0, 100, b->percent, [](double val) {
            services::Brightness::set(static_cast<int>(val));
        }));
    }
}

} // namespace realmheart::ui::sidebar
