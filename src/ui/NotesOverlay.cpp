#include "ui/NotesOverlay.hpp"

#include "ui/LayerSurface.hpp"

#include <algorithm>

namespace realmheart::ui {

namespace {

constexpr unsigned max_geometry_retries = 8;

unsigned geometry_retry_delay(unsigned attempt) {
    constexpr unsigned initial_delay_ms = 50;
    constexpr unsigned max_delay_ms = 2000;
    const unsigned shift = std::min(attempt, 5U);
    return std::min(initial_delay_ms << shift, max_delay_ms);
}

} // namespace

NotesOverlay::NotesOverlay(
    GtkApplication* app,
    services::NotesService* notes_service,
    int monitor_index
) : notes_service_(notes_service),
    monitor_index_(monitor_index) {
    lifetime_->owner = this;

    window_ = GTK_WIDGET(gtk_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Notes");
    gtk_window_set_default_size(
        GTK_WINDOW(window_), layout_.window_width, layout_.window_height
    );
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);

    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-notes";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::Exclusive;
    spec.monitor_index = monitor_index_;
    apply_layer_surface(GTK_WINDOW(window_), spec);
    g_signal_connect(window_, "realize", G_CALLBACK(+[](GtkWidget*, gpointer data) {
        static_cast<NotesOverlay*>(data)->apply_geometry();
    }), this);

    gtk_widget_add_css_class(window_, "realmheart-notes");

    buffer_ = gtk_text_buffer_new(nullptr);
    text_view_ = gtk_text_view_new_with_buffer(buffer_);
    gtk_widget_add_css_class(text_view_, "realmheart-notes-editor");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_), GTK_WRAP_WORD);
    gtk_text_view_set_left_margin(
        GTK_TEXT_VIEW(text_view_), layout_.text_margin_horizontal
    );
    gtk_text_view_set_right_margin(
        GTK_TEXT_VIEW(text_view_), layout_.text_margin_horizontal
    );
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view_), layout_.text_margin_top);
    gtk_text_view_set_bottom_margin(
        GTK_TEXT_VIEW(text_view_), layout_.text_margin_bottom
    );

    if (notes_service_ != nullptr) {
        const std::string content = notes_service_->get_content();
        gtk_text_buffer_set_text(
            buffer_,
            content.data(),
            static_cast<gint>(content.size())
        );
    }

    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text_view_);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(header, "realmheart-notes-header");
    GtkWidget* title = gtk_label_new("NOTES");
    gtk_widget_add_css_class(title, "realmheart-notes-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_box_append(GTK_BOX(header), title);
    status_label_ = gtk_label_new("Saved");
    gtk_widget_add_css_class(status_label_, "realmheart-notes-save-state");
    gtk_widget_set_halign(status_label_, GTK_ALIGN_END);
    gtk_widget_set_valign(status_label_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), status_label_);
    gtk_box_append(GTK_BOX(root), header);

    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(root), scrolled);
    gtk_window_set_child(GTK_WINDOW(window_), root);

    g_signal_connect(buffer_, "changed", G_CALLBACK(on_text_changed_callback), this);

    if (notes_service_ != nullptr) {
        const auto lifetime = lifetime_;
        notes_service_->set_save_state_callback([lifetime](services::NotesSaveState state) {
            struct Payload {
                std::shared_ptr<LifetimeState> lifetime;
                services::NotesSaveState state;
            };
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    if (payload->lifetime->alive.load() &&
                        payload->lifetime->owner != nullptr) {
                        payload->lifetime->owner->apply_save_state(payload->state);
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{lifetime, state},
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        });
        apply_save_state(notes_service_->save_state());
    }

    gtk_widget_set_visible(window_, FALSE);
}

NotesOverlay::~NotesOverlay() {
    cancel_geometry_retry();
    if (notes_service_ != nullptr) notes_service_->set_save_state_callback({});
    lifetime_->alive = false;
    lifetime_->owner = nullptr;
    if (buffer_ != nullptr) g_signal_handlers_disconnect_by_data(buffer_, this);
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
    // gtk_text_view_new_with_buffer() retains its own reference; release the
    // caller-owned reference returned by gtk_text_buffer_new() exactly once.
    if (buffer_ != nullptr) {
        g_object_unref(buffer_);
        buffer_ = nullptr;
    }
}

void NotesOverlay::cancel_geometry_retry() {
    if (geometry_retry_id_ != 0) {
        g_source_remove(geometry_retry_id_);
        geometry_retry_id_ = 0;
    }
    geometry_retry_attempts_ = 0;
}

gboolean NotesOverlay::retry_geometry(gpointer data) {
    auto* self = static_cast<NotesOverlay*>(data);
    self->geometry_retry_id_ = 0;
    self->apply_geometry();
    return G_SOURCE_REMOVE;
}

void NotesOverlay::schedule_geometry_retry() {
    if (window_ == nullptr || !gtk_widget_get_visible(window_) || geometry_retry_id_ != 0 ||
        geometry_retry_attempts_ >= max_geometry_retries) {
        return;
    }
    const unsigned delay = geometry_retry_delay(geometry_retry_attempts_++);
    geometry_retry_id_ = g_timeout_add(delay, &NotesOverlay::retry_geometry, this);
}

void NotesOverlay::apply_geometry() {
    if (window_ == nullptr) return;

    GdkMonitor* monitor = resolve_layer_surface_monitor(window_, monitor_index_);
    if (monitor == nullptr) {
        schedule_geometry_retry();
        return;
    }

    GdkRectangle monitor_geometry{};
    gdk_monitor_get_geometry(monitor, &monitor_geometry);
    g_object_unref(monitor);
    if (monitor_geometry.width <= 0 || monitor_geometry.height <= 0) {
        schedule_geometry_retry();
        return;
    }

    layout_ = notes_layout_for_logical_geometry(
        monitor_geometry.width, monitor_geometry.height
    );
    gtk_window_set_default_size(
        GTK_WINDOW(window_), layout_.window_width, layout_.window_height
    );
    gtk_text_view_set_left_margin(
        GTK_TEXT_VIEW(text_view_), layout_.text_margin_horizontal
    );
    gtk_text_view_set_right_margin(
        GTK_TEXT_VIEW(text_view_), layout_.text_margin_horizontal
    );
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view_), layout_.text_margin_top);
    gtk_text_view_set_bottom_margin(
        GTK_TEXT_VIEW(text_view_), layout_.text_margin_bottom
    );
    geometry_initialized_ = true;
    geometry_retry_attempts_ = 0;
    gtk_widget_queue_resize(window_);
    if (gtk_widget_get_visible(window_)) gtk_widget_set_opacity(window_, 1.0);
}

void NotesOverlay::apply_save_state(services::NotesSaveState state) {
    if (status_label_ == nullptr) return;
    gtk_widget_remove_css_class(status_label_, "pending");
    gtk_widget_remove_css_class(status_label_, "failed");
    switch (state) {
    case services::NotesSaveState::Saved:
        gtk_label_set_text(GTK_LABEL(status_label_), "Saved");
        break;
    case services::NotesSaveState::Pending:
        gtk_label_set_text(GTK_LABEL(status_label_), "Saving…");
        gtk_widget_add_css_class(status_label_, "pending");
        break;
    case services::NotesSaveState::Failed:
        gtk_label_set_text(GTK_LABEL(status_label_), "Save failed");
        gtk_widget_add_css_class(status_label_, "failed");
        break;
    case services::NotesSaveState::DurabilityUncertain:
        gtk_label_set_text(GTK_LABEL(status_label_), "Durability uncertain");
        gtk_widget_add_css_class(status_label_, "failed");
        break;
    case services::NotesSaveState::LoadFailed:
        gtk_label_set_text(GTK_LABEL(status_label_), "Load failed");
        gtk_widget_add_css_class(status_label_, "failed");
        break;
    case services::NotesSaveState::Rejected:
        gtk_label_set_text(GTK_LABEL(status_label_), "Note too large");
        gtk_widget_add_css_class(status_label_, "failed");
        break;
    }
}

void NotesOverlay::on_text_changed_callback(GtkTextBuffer* buf, gpointer data) {
    auto* self = static_cast<NotesOverlay*>(data);
    if (self == nullptr || self->notes_service_ == nullptr || self->suppress_buffer_change_) return;

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char* text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    const std::string content = text != nullptr ? text : "";
    g_free(text);

    if (self->notes_service_->save_state() == services::NotesSaveState::LoadFailed) {
        self->notes_service_->acknowledge_load_failure();
    }
    if (self->notes_service_->set_content(content)) return;

    self->suppress_buffer_change_ = true;
    const std::string accepted = self->notes_service_->get_content();
    gtk_text_buffer_set_text(
        buf,
        accepted.data(),
        static_cast<gint>(accepted.size())
    );
    self->suppress_buffer_change_ = false;
    self->apply_save_state(self->notes_service_->save_state());
}

void NotesOverlay::show() {
    if (window_ == nullptr) return;
    if (!geometry_initialized_) gtk_widget_set_opacity(window_, 0.0);
    gtk_widget_set_visible(window_, TRUE);
    gtk_window_present(GTK_WINDOW(window_));
    apply_geometry();
}

void NotesOverlay::hide() {
    if (window_ == nullptr) return;
    gtk_widget_set_visible(window_, FALSE);
    cancel_geometry_retry();
}

void NotesOverlay::toggle() {
    if (visible()) hide();
    else show();
}

bool NotesOverlay::visible() const {
    return window_ != nullptr && gtk_widget_get_visible(window_);
}

} // namespace realmheart::ui