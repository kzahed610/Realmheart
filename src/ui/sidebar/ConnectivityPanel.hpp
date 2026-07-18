#pragma once

#include "services/Bluetooth.hpp"
#include "services/Wifi.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace realmheart::ui::sidebar {

class WifiManagerPopover {
public:
    explicit WifiManagerPopover(GtkWidget* overlay_host, std::function<void()> state_changed = {});
    ~WifiManagerPopover();

    WifiManagerPopover(const WifiManagerPopover&) = delete;
    WifiManagerPopover& operator=(const WifiManagerPopover&) = delete;

    void show();
    void hide();
    void toggle();
    [[nodiscard]] bool visible() const;
    void refresh(bool rescan = true);

private:
    struct LifetimeState;
    struct RowAction;

    void build();
    void render(
        const std::optional<services::WifiState>& state,
        const std::vector<services::WifiNetwork>& networks
    );
    void show_password_prompt(const services::WifiNetwork& network);
    void hide_password_prompt();
    void connect_network(const services::WifiNetwork& network, std::optional<std::string> password);
    void disconnect_network();
    void forget_network(const services::WifiNetwork& network);
    void set_powered(bool enabled);
    void run_action(std::string progress, std::function<std::string()> action);
    void set_busy(bool busy, const std::string& message = {});
    void set_status(const std::string& message, bool error = false);

    static void on_row_action(GtkButton* button, gpointer data);

    GtkWidget* overlay_host_ = nullptr;
    GtkWidget* backdrop_ = nullptr;
    GtkWidget* revealer_ = nullptr;
    GtkWidget* content_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* status_ = nullptr;
    GtkWidget* spinner_ = nullptr;
    GtkWidget* power_button_ = nullptr;
    GtkWidget* password_revealer_ = nullptr;
    GtkWidget* password_title_ = nullptr;
    GtkWidget* password_entry_ = nullptr;
    std::optional<services::WifiNetwork> pending_network_;
    std::function<void()> state_changed_;
    std::shared_ptr<LifetimeState> lifetime_;
};

class BluetoothManagerPopover {
public:
    explicit BluetoothManagerPopover(GtkWidget* overlay_host, std::function<void()> state_changed = {});
    ~BluetoothManagerPopover();

    BluetoothManagerPopover(const BluetoothManagerPopover&) = delete;
    BluetoothManagerPopover& operator=(const BluetoothManagerPopover&) = delete;

    void show();
    void hide();
    void toggle();
    [[nodiscard]] bool visible() const;
    void refresh(bool scan_for_new_devices = true);

private:
    struct LifetimeState;
    struct RowAction;

    void build();
    void render(
        const std::optional<services::BluetoothState>& state,
        const std::vector<services::BluetoothDevice>& devices
    );
    void connect_device(const services::BluetoothDevice& device);
    void disconnect_device(const services::BluetoothDevice& device);
    void forget_device(const services::BluetoothDevice& device);
    void set_powered(bool powered);
    void run_action(std::string progress, std::function<std::string()> action);
    void set_busy(bool busy, const std::string& message = {});
    void set_status(const std::string& message, bool error = false);

    static void on_row_action(GtkButton* button, gpointer data);

    GtkWidget* overlay_host_ = nullptr;
    GtkWidget* backdrop_ = nullptr;
    GtkWidget* revealer_ = nullptr;
    GtkWidget* content_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* status_ = nullptr;
    GtkWidget* spinner_ = nullptr;
    GtkWidget* power_button_ = nullptr;
    std::function<void()> state_changed_;
    std::shared_ptr<LifetimeState> lifetime_;
};

} // namespace realmheart::ui::sidebar
