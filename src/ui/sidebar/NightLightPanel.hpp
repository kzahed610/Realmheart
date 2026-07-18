#pragma once

#include "services/NightLight.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <mutex>
#include <string>

namespace realmheart::ui::sidebar {

class NightLightPanel {
public:
    explicit NightLightPanel(
        GtkWidget* overlay_host,
        std::function<void()> state_changed = {}
    );
    ~NightLightPanel();

    NightLightPanel(const NightLightPanel&) = delete;
    NightLightPanel& operator=(const NightLightPanel&) = delete;

    void show();
    void hide();
    void toggle();
    [[nodiscard]] bool visible() const;
    void refresh();

private:
    struct LifetimeState;

    void build();
    void render(const services::NightLightState& state);
    void set_enabled(bool enabled);
    void set_temperature(int temperature);
    void run_mutation(std::function<services::NightLightMutationResult()> mutation);
    void update_strength_copy(int strength);
    void set_busy(bool busy);
    void set_status(const std::string& message, bool error = false);

    GtkWidget* overlay_host_ = nullptr;
    GtkWidget* backdrop_ = nullptr;
    GtkWidget* revealer_ = nullptr;
    GtkWidget* content_ = nullptr;
    GtkWidget* toggle_ = nullptr;
    GtkWidget* scale_ = nullptr;
    GtkWidget* strength_value_ = nullptr;
    GtkWidget* temperature_value_ = nullptr;
    GtkWidget* status_ = nullptr;
    GtkWidget* spinner_ = nullptr;
    bool updating_ = false;
    bool enabled_ = false;
    int pending_strength_ = 57;
    guint debounce_source_ = 0;
    std::function<void()> state_changed_;
    std::shared_ptr<LifetimeState> lifetime_;
};

} // namespace realmheart::ui::sidebar
