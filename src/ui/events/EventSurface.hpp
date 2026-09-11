#pragma once

#include "events/EventClient.hpp"
#include "events/EventTypes.hpp"
#include "ui/events/EventSurfaceGeometry.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace realmheart::ui::events {

class EventSurface {
public:
    explicit EventSurface(GtkApplication* application);
    ~EventSurface();
    EventSurface(const EventSurface&) = delete;
    EventSurface& operator=(const EventSurface&) = delete;

private:
    struct Card;
    struct AsyncState;

    void handle_message(const realmheart::events::Json& message);
    void replace_snapshot(const realmheart::events::Json& events);
    void upsert_event(const realmheart::events::Event& event);
    void remove_event(const realmheart::events::EventKey& key);
    void erase_event_now(const realmheart::events::EventKey& key);
    void finish_pending_removal();
    void reorder_cards();
    void rebuild_selector(const std::vector<Card*>& ordered);
    void select_event(const realmheart::events::EventKey& key);
    void refresh_visibility();
    void refresh_geometry();
    void refresh_primary_presentation();
    void on_revealer_concealed();
    void send_user_operation(const std::string& op, const realmheart::events::EventKey& key);
    void activate_producer_action(const realmheart::events::EventKey& key, const realmheart::events::EventAction& action);

    [[nodiscard]] std::vector<Card*> ordered_cards() const;
    [[nodiscard]] Card* primary_card() const;

    std::unique_ptr<Card> create_card(const realmheart::events::Event& event);
    void update_card(Card& card, const realmheart::events::Event& event);

    GtkApplication* application_ = nullptr;
    GtkWindow* window_ = nullptr;
    GtkWidget* surface_revealer_ = nullptr;
    GtkWidget* surface_scroller_ = nullptr;
    GtkWidget* surface_frame_ = nullptr;
    GtkWidget* selector_section_ = nullptr;
    GtkWidget* selector_count_ = nullptr;
    GtkWidget* selector_box_ = nullptr;
    GtkWidget* card_stack_ = nullptr;
    std::unordered_map<realmheart::events::EventKey, std::unique_ptr<Card>, realmheart::events::EventKeyHash> cards_;
    std::optional<realmheart::events::EventKey> primary_key_;
    std::optional<realmheart::events::EventKey> pending_removal_;
    bool primary_user_selected_ = false;
    bool replacing_snapshot_ = false;
    EventSurfaceGeometry geometry_{};
    realmheart::events::EventSubscriber subscriber_;
    std::shared_ptr<AsyncState> async_state_;
};

} // namespace realmheart::ui::events
