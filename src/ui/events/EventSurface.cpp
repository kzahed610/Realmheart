#include "ui/events/EventSurface.hpp"
#include "ui/events/EventRevealClip.hpp"

#include "events/EventProtocol.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/MonitorResolver.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace realmheart::ui::events {
namespace {

using Event = realmheart::events::Event;
using EventKey = realmheart::events::EventKey;
using Json = realmheart::events::Json;

GtkWidget* make_label(const char* css_class, float xalign = 0.0F) {
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label), xalign);
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(label, css_class);
    return label;
}

void set_label(GtkWidget* label, const std::string& text) {
    gtk_label_set_text(GTK_LABEL(label), text.c_str());
}

void clear_box(GtkWidget* box) {
    while (GtkWidget* child = gtk_widget_get_first_child(box)) {
        gtk_box_remove(GTK_BOX(box), child);
    }
}

void clear_flow_box(GtkWidget* flow_box) {
    gtk_flow_box_remove_all(GTK_FLOW_BOX(flow_box));
}

int presentation_rank(realmheart::events::Presentation presentation) {
    switch (presentation) {
    case realmheart::events::Presentation::Persistent: return 3;
    case realmheart::events::Presentation::Attention: return 2;
    case realmheart::events::Presentation::Ambient: return 1;
    }
    return 0;
}

int severity_rank(realmheart::events::Severity severity) {
    switch (severity) {
    case realmheart::events::Severity::Critical: return 4;
    case realmheart::events::Severity::Warning: return 3;
    case realmheart::events::Severity::Success: return 2;
    case realmheart::events::Severity::Info: return 1;
    }
    return 0;
}

bool higher_attention_class(const Event& left, const Event& right) {
    const int left_presentation = presentation_rank(left.presentation);
    const int right_presentation = presentation_rank(right.presentation);
    if (left_presentation != right_presentation) {
        return left_presentation > right_presentation;
    }
    return severity_rank(left.severity) > severity_rank(right.severity);
}

bool event_precedes(const Event& left, const Event& right) {
    const int left_presentation = presentation_rank(left.presentation);
    const int right_presentation = presentation_rank(right.presentation);
    if (left_presentation != right_presentation) {
        return left_presentation > right_presentation;
    }
    const int left_severity = severity_rank(left.severity);
    const int right_severity = severity_rank(right.severity);
    if (left_severity != right_severity) return left_severity > right_severity;
    return left.revision > right.revision;
}

std::string uppercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        if (value >= 'a' && value <= 'z') return static_cast<char>(value - ('a' - 'A'));
        return static_cast<char>(value);
    });
    return text;
}

std::string severity_text(realmheart::events::Severity severity) {
    return uppercase(realmheart::events::to_string(severity));
}

std::string source_text(const Event& event) {
    return event.source.name.empty() ? event.source.id : event.source.name;
}

std::string severity_css_class(
    const char* prefix,
    realmheart::events::Severity severity
) {
    return std::string(prefix) + realmheart::events::to_string(severity);
}

guint reveal_duration_ms() {
    gboolean animations_enabled = TRUE;
    if (GtkSettings* settings = gtk_settings_get_default(); settings != nullptr) {
        g_object_get(settings, "gtk-enable-animations", &animations_enabled, nullptr);
    }
    return animations_enabled ? 300U : 0U;
}

} // namespace

struct EventSurface::AsyncState {
    std::atomic<EventSurface*> owner{nullptr};
};

struct EventSurface::Card {
    Event event;
    GtkWidget* root = nullptr;
    GtkWidget* source = nullptr;
    GtkWidget* severity = nullptr;
    GtkWidget* title = nullptr;
    GtkWidget* summary = nullptr;
    GtkWidget* fields = nullptr;
    GtkWidget* progress_box = nullptr;
    GtkWidget* progress = nullptr;
    GtkWidget* progress_label = nullptr;
    GtkWidget* details_button = nullptr;
    GtkWidget* details_revealer = nullptr;
    GtkWidget* details_scroller = nullptr;
    GtkWidget* details_view = nullptr;
    GtkWidget* producer_actions = nullptr;
    GtkWidget* dismiss = nullptr;
    GtkWidget* timestamp = nullptr;
};

struct LifecycleActionContext {
    EventSurface* owner = nullptr;
    EventKey key;
    std::string op;
};

struct ProducerActionContext {
    EventSurface* owner = nullptr;
    EventKey key;
    realmheart::events::EventAction action;
};

struct SelectorContext {
    EventSurface* owner = nullptr;
    EventKey key;
};

EventSurface::EventSurface(GtkApplication* application)
    : application_(application), async_state_(std::make_shared<AsyncState>()) {
    async_state_->owner.store(this);

    window_ = GTK_WINDOW(gtk_application_window_new(application_));
    gtk_widget_add_css_class(GTK_WIDGET(window_), "realmheart-event-surface-window");
    gtk_window_set_decorated(window_, false);
    gtk_window_set_resizable(window_, false);

    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    const int initial_monitor = display != nullptr ? focused_monitor_index(display) : 0;
    const auto initial_context = display != nullptr
        ? monitor_context_for_index(display, initial_monitor)
        : std::nullopt;
    geometry_ = event_surface_geometry_for_monitor(
        initial_context.value_or(core::MonitorContext{})
    );

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-event-surface";
    spec.layer = LayerSurfaceLevel::Top;
    spec.keyboard_mode = LayerKeyboardMode::OnDemand;
    spec.monitor_index = initial_monitor;
    spec.anchor_top = true;
    spec.margin_top = geometry_.top_margin;
    apply_layer_surface(window_, spec);

    surface_frame_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(surface_frame_, "realmheart-event-frame");

    selector_section_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(selector_section_, "realmheart-event-selector-section");
    selector_count_ = make_label("realmheart-event-selector-count");
    gtk_box_append(GTK_BOX(selector_section_), selector_count_);

    GtkWidget* selector_scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(selector_scroller, "realmheart-event-selector-scroller");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(selector_scroller),
        GTK_POLICY_AUTOMATIC,
        GTK_POLICY_NEVER
    );
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(selector_scroller),
        true
    );
    selector_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(selector_box_, "realmheart-event-selector-strip");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(selector_scroller), selector_box_);
    gtk_box_append(GTK_BOX(selector_section_), selector_scroller);
    gtk_box_append(GTK_BOX(surface_frame_), selector_section_);

    card_stack_ = gtk_stack_new();
    gtk_widget_add_css_class(card_stack_, "realmheart-event-primary-stack");
    gtk_stack_set_transition_type(GTK_STACK(card_stack_), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(card_stack_), 140);
    gtk_stack_set_hhomogeneous(GTK_STACK(card_stack_), true);
    gtk_stack_set_vhomogeneous(GTK_STACK(card_stack_), false);
    gtk_box_append(GTK_BOX(surface_frame_), card_stack_);

    surface_scroller_ = gtk_scrolled_window_new();
    gtk_widget_add_css_class(surface_scroller_, "realmheart-event-surface-scroller");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(surface_scroller_),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC
    );
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(surface_scroller_),
        true
    );
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(surface_scroller_), surface_frame_);

    const guint surface_reveal_duration = reveal_duration_ms();
    surface_revealer_ = realmheart_event_reveal_clip_new(
        surface_scroller_,
        surface_reveal_duration,
        surface_reveal_duration
    );
    gtk_widget_add_css_class(surface_revealer_, "realmheart-event-surface-revealer");
    realmheart_event_reveal_clip_set_revealed_immediately(
        REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_),
        false
    );
    gtk_window_set_child(window_, surface_revealer_);
    gtk_widget_set_visible(GTK_WIDGET(window_), false);

    g_signal_connect(
        surface_revealer_,
        "concealed",
        G_CALLBACK(+[](RealmheartEventRevealClip*, gpointer data) {
            static_cast<EventSurface*>(data)->on_revealer_concealed();
        }),
        this
    );
    g_signal_connect(
        GTK_WIDGET(window_),
        "realize",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            static_cast<EventSurface*>(data)->refresh_geometry();
        }),
        this
    );

    refresh_geometry();

    const auto state = async_state_;
    subscriber_.start(
        [state](const Json& message) {
            struct PendingMessage {
                std::shared_ptr<AsyncState> state;
                Json message;
            };
            auto* pending = new PendingMessage{state, message};
            g_main_context_invoke(nullptr, +[](gpointer data) -> gboolean {
                std::unique_ptr<PendingMessage> pending(static_cast<PendingMessage*>(data));
                if (auto* owner = pending->state->owner.load(); owner != nullptr) {
                    owner->handle_message(pending->message);
                }
                return G_SOURCE_REMOVE;
            }, pending);
        }
    );
}

EventSurface::~EventSurface() {
    async_state_->owner.store(nullptr);
    subscriber_.stop();
    cards_.clear();
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
        window_ = nullptr;
    }
}

std::unique_ptr<EventSurface::Card> EventSurface::create_card(const Event& event) {
    auto card = std::make_unique<Card>();
    card->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card->root, "realmheart-event-card");

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header, "realmheart-event-header");
    card->source = make_label("realmheart-event-source");
    gtk_widget_set_hexpand(card->source, true);
    card->severity = make_label("realmheart-event-severity", 1.0F);
    gtk_label_set_wrap(GTK_LABEL(card->severity), false);
    gtk_label_set_single_line_mode(GTK_LABEL(card->severity), true);
    gtk_widget_set_halign(card->severity, GTK_ALIGN_END);
    gtk_widget_set_valign(card->severity, GTK_ALIGN_START);
    gtk_widget_set_hexpand(card->severity, false);
    gtk_box_append(GTK_BOX(header), card->source);
    gtk_box_append(GTK_BOX(header), card->severity);
    gtk_box_append(GTK_BOX(card->root), header);

    card->title = make_label("realmheart-event-title");
    card->summary = make_label("realmheart-event-summary");
    gtk_box_append(GTK_BOX(card->root), card->title);
    gtk_box_append(GTK_BOX(card->root), card->summary);

    card->fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card->fields, "realmheart-event-fields");
    gtk_box_append(GTK_BOX(card->root), card->fields);

    card->progress_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(card->progress_box, "realmheart-event-progress-row");
    card->progress = gtk_progress_bar_new();
    gtk_widget_set_hexpand(card->progress, true);
    card->progress_label = make_label("realmheart-event-progress-label", 1.0F);
    gtk_box_append(GTK_BOX(card->progress_box), card->progress);
    gtk_box_append(GTK_BOX(card->progress_box), card->progress_label);
    gtk_box_append(GTK_BOX(card->root), card->progress_box);

    card->details_button = gtk_button_new_with_label("▾ Diagnostic details");
    gtk_widget_add_css_class(card->details_button, "realmheart-event-details-button");
    gtk_widget_set_halign(card->details_button, GTK_ALIGN_START);
    card->details_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(
        GTK_REVEALER(card->details_revealer),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN
    );
    gtk_revealer_set_transition_duration(
        GTK_REVEALER(card->details_revealer),
        reveal_duration_ms() / 2U
    );

    card->details_scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(card->details_scroller, "realmheart-event-details-shell");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(card->details_scroller),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC
    );
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(card->details_scroller),
        true
    );
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(card->details_scroller),
        geometry_.details_max_height
    );

    card->details_view = gtk_text_view_new();
    gtk_widget_add_css_class(card->details_view, "realmheart-event-details-view");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(card->details_view), false);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(card->details_view), false);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(card->details_view), true);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(card->details_view), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(
        GTK_SCROLLED_WINDOW(card->details_scroller),
        card->details_view
    );
    gtk_revealer_set_child(
        GTK_REVEALER(card->details_revealer),
        card->details_scroller
    );
    g_signal_connect(card->details_button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
        auto* target = static_cast<Card*>(data);
        const bool reveal = !gtk_revealer_get_reveal_child(GTK_REVEALER(target->details_revealer));
        gtk_revealer_set_reveal_child(GTK_REVEALER(target->details_revealer), reveal);
        gtk_button_set_label(button, reveal ? "▴ Hide diagnostic details" : "▾ Diagnostic details");
    }), card.get());
    gtk_box_append(GTK_BOX(card->root), card->details_button);
    gtk_box_append(GTK_BOX(card->root), card->details_revealer);

    card->producer_actions = gtk_flow_box_new();
    gtk_widget_add_css_class(card->producer_actions, "realmheart-event-producer-actions");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(card->producer_actions), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(card->producer_actions), false);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(card->producer_actions), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(card->producer_actions), 8);
    gtk_box_append(GTK_BOX(card->root), card->producer_actions);

    GtkWidget* lifecycle_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(lifecycle_actions, "realmheart-event-actions");
    gtk_widget_set_halign(lifecycle_actions, GTK_ALIGN_END);
    card->dismiss = gtk_button_new_with_label("Dismiss ×");
    gtk_widget_add_css_class(card->dismiss, "realmheart-event-action");
    gtk_widget_add_css_class(card->dismiss, "realmheart-event-dismiss");
    gtk_box_append(GTK_BOX(lifecycle_actions), card->dismiss);
    gtk_box_append(GTK_BOX(card->root), lifecycle_actions);

    card->timestamp = make_label("realmheart-event-timestamp");
    gtk_box_append(GTK_BOX(card->root), card->timestamp);

    auto* dismiss_context = new LifecycleActionContext{
        this,
        {event.source.id, event.id},
        "dismiss"
    };
    g_object_set_data_full(
        G_OBJECT(card->dismiss),
        "realmheart-event-action",
        dismiss_context,
        +[](gpointer data) { delete static_cast<LifecycleActionContext*>(data); }
    );
    g_signal_connect(card->dismiss, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer) {
        auto* context = static_cast<LifecycleActionContext*>(
            g_object_get_data(G_OBJECT(button), "realmheart-event-action")
        );
        if (context != nullptr && context->owner != nullptr) {
            context->owner->send_user_operation(context->op, context->key);
        }
    }), nullptr);

    update_card(*card, event);
    return card;
}

void EventSurface::update_card(Card& card, const Event& event) {
    gtk_widget_remove_css_class(card.root, "realmheart-event-card-info");
    gtk_widget_remove_css_class(card.root, "realmheart-event-card-success");
    gtk_widget_remove_css_class(card.root, "realmheart-event-card-warning");
    gtk_widget_remove_css_class(card.root, "realmheart-event-card-critical");
    gtk_widget_add_css_class(
        card.root,
        severity_css_class("realmheart-event-card-", event.severity).c_str()
    );

    set_label(card.source, source_text(event));
    set_label(card.severity, severity_text(event.severity));
    set_label(card.title, event.title);
    set_label(card.summary, event.summary);
    gtk_widget_set_visible(card.summary, !event.summary.empty());

    clear_box(card.fields);
    for (const auto& field : event.fields) {
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(row, "realmheart-event-field-row");
        GtkWidget* label = make_label("realmheart-event-field-label");
        GtkWidget* value = make_label("realmheart-event-field-value", 1.0F);
        set_label(label, field.label);
        set_label(value, field.value);
        gtk_widget_set_hexpand(value, true);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), value);
        gtk_box_append(GTK_BOX(card.fields), row);
    }
    gtk_widget_set_visible(card.fields, !event.fields.empty());

    const bool has_progress = event.progress &&
        event.progress->mode != realmheart::events::ProgressMode::None;
    gtk_widget_set_visible(card.progress_box, has_progress);
    if (has_progress) {
        if (event.progress->mode == realmheart::events::ProgressMode::Determinate) {
            gtk_progress_bar_set_fraction(
                GTK_PROGRESS_BAR(card.progress),
                std::clamp(event.progress->value, 0.0, 1.0)
            );
        } else {
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(card.progress));
        }
        std::string label = event.progress->label;
        if (label.empty() && event.progress->mode == realmheart::events::ProgressMode::Determinate) {
            label = std::to_string(
                static_cast<int>(std::round(event.progress->value * 100.0))
            ) + "%";
        }
        set_label(card.progress_label, label);
        gtk_widget_set_visible(card.progress_label, !label.empty());
    }

    const bool has_details = event.details && !event.details->text.empty();
    gtk_widget_set_visible(card.details_button, has_details);
    gtk_widget_set_visible(card.details_revealer, has_details);
    if (has_details) {
        GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(card.details_view));
        gtk_text_buffer_set_text(buffer, event.details->text.c_str(), -1);
    } else {
        gtk_revealer_set_reveal_child(GTK_REVEALER(card.details_revealer), false);
        gtk_button_set_label(GTK_BUTTON(card.details_button), "▾ Diagnostic details");
    }

    clear_flow_box(card.producer_actions);
    for (const auto& action : event.actions) {
        GtkWidget* button = gtk_button_new_with_label(action.label.c_str());
        gtk_widget_add_css_class(button, "realmheart-event-action");
        gtk_widget_add_css_class(button, "realmheart-event-producer-action");
        gtk_widget_add_css_class(
            button,
            ("realmheart-event-action-" + realmheart::events::to_string(action.kind)).c_str()
        );
        auto* context = new ProducerActionContext{
            this,
            {event.source.id, event.id},
            action
        };
        g_object_set_data_full(
            G_OBJECT(button),
            "realmheart-event-producer-action",
            context,
            +[](gpointer data) { delete static_cast<ProducerActionContext*>(data); }
        );
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer) {
            auto* context = static_cast<ProducerActionContext*>(
                g_object_get_data(G_OBJECT(clicked), "realmheart-event-producer-action")
            );
            if (context != nullptr && context->owner != nullptr) {
                context->owner->activate_producer_action(context->key, context->action);
            }
        }), nullptr);
        gtk_flow_box_append(GTK_FLOW_BOX(card.producer_actions), button);
    }
    gtk_widget_set_visible(card.producer_actions, !event.actions.empty());

    std::string footer = uppercase(realmheart::events::to_string(event.presentation));
    if (!event.timestamp.empty()) footer += "  ·  " + event.timestamp;
    set_label(card.timestamp, footer);
    card.event = event;
}

void EventSurface::handle_message(const Json& message) {
    if (!message.is_object()) return;
    const std::string type = message.value("type", "");
    if (type == "snapshot") {
        if (message.contains("events")) replace_snapshot(message["events"]);
        return;
    }
    if (!message.contains("event")) return;
    realmheart::events::ValidationResult validation;
    const auto event = realmheart::events::event_from_json(message["event"], validation);
    if (!event) return;
    const EventKey key{event->source.id, event->id};
    if (type == "EVENT_RESOLVED" || type == "EVENT_DELETED" || type == "EVENT_DISMISSED") {
        remove_event(key);
    } else if (type == "EVENT_CREATED" || type == "EVENT_UPDATED" || type == "EVENT_ACKNOWLEDGED") {
        upsert_event(*event);
    }
}

void EventSurface::replace_snapshot(const Json& events_json) {
    replacing_snapshot_ = true;
    pending_removal_.reset();
    primary_key_.reset();
    primary_user_selected_ = false;

    for (auto& [key, card] : cards_) {
        static_cast<void>(key);
        if (card->root != nullptr && gtk_widget_get_parent(card->root) == card_stack_) {
            gtk_stack_remove(GTK_STACK(card_stack_), card->root);
        }
    }
    cards_.clear();

    if (events_json.is_array()) {
        for (const auto& item : events_json) {
            realmheart::events::ValidationResult validation;
            const auto event = realmheart::events::event_from_json(item, validation);
            if (event) upsert_event(*event);
        }
    }

    replacing_snapshot_ = false;
    reorder_cards();
    refresh_visibility();
}

void EventSurface::upsert_event(const Event& event) {
    const EventKey key{event.source.id, event.id};
    const auto it = cards_.find(key);
    if (it == cards_.end()) {
        auto card = create_card(event);
        gtk_stack_add_child(GTK_STACK(card_stack_), card->root);
        cards_.emplace(key, std::move(card));
    } else if (event.revision >= it->second->event.revision) {
        update_card(*it->second, event);
    }

    if (replacing_snapshot_) return;
    reorder_cards();
    refresh_visibility();
}

void EventSurface::remove_event(const EventKey& key) {
    if (pending_removal_ && *pending_removal_ == key) return;
    const auto it = cards_.find(key);
    if (it == cards_.end()) return;

    const bool is_primary = primary_key_ && *primary_key_ == key;
    const bool can_animate = is_primary &&
        gtk_widget_get_visible(GTK_WIDGET(window_)) &&
        realmheart_event_reveal_clip_get_revealed(REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_));

    if (can_animate && !pending_removal_) {
        pending_removal_ = key;
        gtk_widget_add_css_class(surface_frame_, "realmheart-event-frame-furling");
        realmheart_event_reveal_clip_set_revealed(REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_), false);
        return;
    }

    erase_event_now(key);
    reorder_cards();
    refresh_visibility();
}

void EventSurface::erase_event_now(const EventKey& key) {
    const auto it = cards_.find(key);
    if (it == cards_.end()) return;
    if (it->second->root != nullptr && gtk_widget_get_parent(it->second->root) == card_stack_) {
        gtk_stack_remove(GTK_STACK(card_stack_), it->second->root);
    }
    cards_.erase(it);
    if (primary_key_ && *primary_key_ == key) {
        primary_key_.reset();
        primary_user_selected_ = false;
    }
}

void EventSurface::finish_pending_removal() {
    if (!pending_removal_) return;
    const EventKey key = *pending_removal_;
    pending_removal_.reset();
    erase_event_now(key);
    reorder_cards();
    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-furling");
    refresh_visibility();
}

std::vector<EventSurface::Card*> EventSurface::ordered_cards() const {
    std::vector<Card*> ordered;
    ordered.reserve(cards_.size());
    for (const auto& [key, card] : cards_) {
        static_cast<void>(key);
        ordered.push_back(card.get());
    }
    std::sort(ordered.begin(), ordered.end(), [](const Card* left, const Card* right) {
        return event_precedes(left->event, right->event);
    });
    return ordered;
}

EventSurface::Card* EventSurface::primary_card() const {
    if (!primary_key_) return nullptr;
    const auto it = cards_.find(*primary_key_);
    return it == cards_.end() ? nullptr : it->second.get();
}

void EventSurface::reorder_cards() {
    const auto ordered = ordered_cards();
    if (ordered.empty()) {
        primary_key_.reset();
        primary_user_selected_ = false;
        clear_box(selector_box_);
        gtk_widget_set_visible(selector_section_, false);
        return;
    }

    Card* current = primary_card();
    Card* highest = ordered.front();
    if (current == nullptr) {
        primary_key_ = EventKey{highest->event.source.id, highest->event.id};
        primary_user_selected_ = false;
    } else if (!primary_user_selected_) {
        primary_key_ = EventKey{highest->event.source.id, highest->event.id};
    } else if (higher_attention_class(highest->event, current->event)) {
        primary_key_ = EventKey{highest->event.source.id, highest->event.id};
        primary_user_selected_ = false;
    }

    refresh_primary_presentation();
    rebuild_selector(ordered);
}

void EventSurface::rebuild_selector(const std::vector<Card*>& ordered) {
    clear_box(selector_box_);
    if (ordered.size() <= 1 || !primary_key_) {
        gtk_widget_set_visible(selector_section_, false);
        return;
    }

    const std::size_t other_count = ordered.size() - 1;
    set_label(
        selector_count_,
        std::to_string(other_count) +
            (other_count == 1 ? " OTHER ACTIVE EVENT" : " OTHER ACTIVE EVENTS")
    );

    for (Card* card : ordered) {
        const EventKey key{card->event.source.id, card->event.id};
        if (key == *primary_key_) continue;

        std::string label = source_text(card->event) + "  ·  " + card->event.title;
        GtkWidget* button = gtk_button_new_with_label(label.c_str());
        gtk_widget_add_css_class(button, "realmheart-event-selector-chip");
        gtk_widget_add_css_class(
            button,
            severity_css_class("realmheart-event-selector-", card->event.severity).c_str()
        );
        if (GtkWidget* child = gtk_button_get_child(GTK_BUTTON(button)); GTK_IS_LABEL(child)) {
            gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_END);
            gtk_label_set_single_line_mode(GTK_LABEL(child), true);
            gtk_label_set_xalign(GTK_LABEL(child), 0.0F);
        }

        auto* context = new SelectorContext{this, key};
        g_object_set_data_full(
            G_OBJECT(button),
            "realmheart-event-selector",
            context,
            +[](gpointer data) { delete static_cast<SelectorContext*>(data); }
        );
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer) {
            auto* context = static_cast<SelectorContext*>(
                g_object_get_data(G_OBJECT(clicked), "realmheart-event-selector")
            );
            if (context != nullptr && context->owner != nullptr) {
                context->owner->select_event(context->key);
            }
        }), nullptr);
        gtk_box_append(GTK_BOX(selector_box_), button);
    }

    gtk_widget_set_visible(selector_section_, true);
}

void EventSurface::select_event(const EventKey& key) {
    if (!cards_.contains(key)) return;
    primary_key_ = key;
    primary_user_selected_ = true;
    reorder_cards();
}

void EventSurface::refresh_primary_presentation() {
    Card* card = primary_card();
    if (card == nullptr) return;

    gtk_stack_set_visible_child(GTK_STACK(card_stack_), card->root);

    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-info");
    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-success");
    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-warning");
    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-critical");
    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-ambient");
    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-attention");
    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-persistent");

    gtk_widget_add_css_class(
        surface_frame_,
        severity_css_class("realmheart-event-frame-", card->event.severity).c_str()
    );
    gtk_widget_add_css_class(
        surface_frame_,
        ("realmheart-event-frame-" + realmheart::events::to_string(card->event.presentation)).c_str()
    );
}

void EventSurface::refresh_visibility() {
    if (cards_.empty()) {
        if (gtk_widget_get_visible(GTK_WIDGET(window_)) &&
            realmheart_event_reveal_clip_get_revealed(REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_))) {
            gtk_widget_add_css_class(surface_frame_, "realmheart-event-frame-furling");
            realmheart_event_reveal_clip_set_revealed(REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_), false);
        } else if (realmheart_event_reveal_clip_is_concealed(
                       REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_))) {
            gtk_widget_set_visible(GTK_WIDGET(window_), false);
            gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-furling");
        }
        return;
    }

    if (pending_removal_) return;
    gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-furling");
    refresh_geometry();
    if (!gtk_widget_get_visible(GTK_WIDGET(window_))) {
        realmheart_event_reveal_clip_set_revealed_immediately(
            REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_),
            false
        );
        gtk_window_present(window_);
        auto* reveal_state = new std::shared_ptr<AsyncState>(async_state_);
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer data) -> gboolean {
                std::unique_ptr<std::shared_ptr<AsyncState>> state(
                    static_cast<std::shared_ptr<AsyncState>*>(data)
                );
                if (auto* owner = (*state)->owner.load();
                    owner != nullptr && !owner->cards_.empty() && !owner->pending_removal_) {
                    realmheart_event_reveal_clip_set_revealed(
                        REALMHEART_EVENT_REVEAL_CLIP(owner->surface_revealer_),
                        true
                    );
                }
                return G_SOURCE_REMOVE;
            },
            reveal_state,
            nullptr
        );
    } else if (!realmheart_event_reveal_clip_get_revealed(REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_))) {
        realmheart_event_reveal_clip_set_revealed(REALMHEART_EVENT_REVEAL_CLIP(surface_revealer_), true);
    }
}

void EventSurface::refresh_geometry() {
    if (window_ == nullptr || surface_scroller_ == nullptr) return;
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    if (display == nullptr) return;

    const int monitor_index = focused_monitor_index(display);
    const auto context = monitor_context_for_index(display, monitor_index);
    if (!context) return;

    geometry_ = event_surface_geometry_for_monitor(*context);
    gtk_widget_set_size_request(surface_scroller_, geometry_.surface_width, -1);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(surface_scroller_),
        geometry_.max_surface_height
    );

    for (auto& [key, card] : cards_) {
        static_cast<void>(key);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(card->details_scroller),
            geometry_.details_max_height
        );
        const guint gap = static_cast<guint>(std::max(1, geometry_.top_margin / 3));
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(card->producer_actions), gap);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(card->producer_actions), gap);
    }

    if (gtk_widget_get_realized(GTK_WIDGET(window_))) {
        static_cast<void>(set_layer_surface_monitor(window_, monitor_index));
        set_layer_surface_margins(window_, 0, 0, geometry_.top_margin, 0);
    }
}

void EventSurface::on_revealer_concealed() {
    if (pending_removal_) {
        finish_pending_removal();
        return;
    }

    if (cards_.empty()) {
        gtk_widget_set_visible(GTK_WIDGET(window_), false);
        gtk_widget_remove_css_class(surface_frame_, "realmheart-event-frame-furling");
    }
}

void EventSurface::send_user_operation(const std::string& op, const EventKey& key) {
    Json request{
        {"protocol", realmheart::events::kProtocolVersion},
        {"op", op},
        {"source_id", key.source_id},
        {"event_id", key.event_id}
    };
    std::string error;
    static_cast<void>(realmheart::events::EventClient::request(request, error));
}

void EventSurface::activate_producer_action(
    const EventKey& key,
    const realmheart::events::EventAction& action
) {
    switch (action.kind) {
    case realmheart::events::ActionKind::Copy: {
        GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
        if (display == nullptr) return;
        GdkClipboard* clipboard = gdk_display_get_clipboard(display);
        if (clipboard != nullptr) gdk_clipboard_set_text(clipboard, action.value.c_str());
        return;
    }
    case realmheart::events::ActionKind::Uri: {
        GError* error = nullptr;
        if (!g_app_info_launch_default_for_uri(action.uri.c_str(), nullptr, &error) && error != nullptr) {
            g_warning("Realmheart Event Surface: unable to open action URI: %s", error->message);
            g_error_free(error);
        }
        return;
    }
    case realmheart::events::ActionKind::Registered: {
        Json request{
            {"protocol", realmheart::events::kProtocolVersion},
            {"op", "invoke_action"},
            {"source_id", key.source_id},
            {"event_id", key.event_id},
            {"action_id", action.id}
        };
        std::string error;
        const Json response = realmheart::events::EventClient::request(request, error);
        if (!response.value("ok", false)) {
            const std::string message = response.contains("error")
                ? response["error"].value("message", "registered action failed")
                : "registered action failed";
            g_warning("Realmheart Event Surface: %s", message.c_str());
        }
        return;
    }
    }
}

} // namespace realmheart::ui::events
