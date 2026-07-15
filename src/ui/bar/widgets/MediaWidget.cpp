#include "ui/bar/widgets/MediaWidget.hpp"

#include "core/TaskExecutor.hpp"
#include "ui/bar/MediaArtLoader.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace realmheart::ui::bar::widgets {
namespace {

GtkWidget* image_button(GtkWidget* icon, const char* fallback) {
    GtkWidget* button = gtk_button_new();
    gtk_widget_add_css_class(button, "realmheart-media-control");
    gtk_button_set_child(
        GTK_BUTTON(button),
        icon != nullptr ? icon : gtk_label_new(fallback)
    );
    return button;
}

} // namespace

MediaWidget::MediaWidget(
    services::MediaService& media_service,
    std::function<void(GtkPopover*)> request_exclusive_open
) : media_service_(media_service),
    request_exclusive_open_(std::move(request_exclusive_open)),
    button_(
        "Realmheart-Icons/media.svg",
        "Md",
        "Media",
        [this] { toggle(); }
    ) {
    async_state_->owner = this;
    button_.add_css_class("realmheart-media-button");

    popover_ = gtk_popover_new();
    gtk_widget_add_css_class(popover_, "realmheart-bar-popover");
    gtk_widget_add_css_class(popover_, "realmheart-media-popover");
    gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_RIGHT);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(popover_), TRUE);
    gtk_popover_set_offset(GTK_POPOVER(popover_), 9, -7);
    gtk_widget_set_parent(popover_, button_.button());

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(root, 220, -1);

    album_stack_ = gtk_stack_new();
    gtk_widget_add_css_class(album_stack_, "realmheart-media-art");
    gtk_widget_set_size_request(album_stack_, 204, 116);
    album_picture_ = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(album_picture_), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(album_picture_, 204, 116);
    album_fallback_ = gtk_label_new("No album art");
    gtk_widget_add_css_class(album_fallback_, "realmheart-media-art-fallback");
    gtk_stack_add_named(GTK_STACK(album_stack_), album_picture_, "picture");
    gtk_stack_add_named(GTK_STACK(album_stack_), album_fallback_, "fallback");
    gtk_stack_set_visible_child_name(GTK_STACK(album_stack_), "fallback");

    title_label_ = gtk_label_new("No active media");
    gtk_widget_add_css_class(title_label_, "realmheart-media-title");
    gtk_label_set_xalign(GTK_LABEL(title_label_), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(title_label_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title_label_), 28);

    artist_label_ = gtk_label_new("");
    gtk_widget_add_css_class(artist_label_, "realmheart-media-artist");
    gtk_label_set_xalign(GTK_LABEL(artist_label_), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(artist_label_), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(artist_label_), 30);

    GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(controls, GTK_ALIGN_CENTER);
    previous_icon_ = std::make_unique<ThemedSvgIcon>(
        "Realmheart-Icons/previous.svg", 18
    );
    previous_button_ = image_button(previous_icon_->widget(), "⏮");

    play_pause_icon_ = std::make_unique<ThemedSvgIcon>(
        "Realmheart-Icons/play.svg", 18
    );
    play_pause_button_ = image_button(play_pause_icon_->widget(), "▶");

    next_icon_ = std::make_unique<ThemedSvgIcon>(
        "Realmheart-Icons/next.svg", 18
    );
    next_button_ = image_button(next_icon_->widget(), "⏭");

    g_signal_connect(previous_button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<MediaWidget*>(data)->invoke_control("previous");
    }), this);
    g_signal_connect(play_pause_button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<MediaWidget*>(data)->invoke_control("play-pause");
    }), this);
    g_signal_connect(next_button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<MediaWidget*>(data)->invoke_control("next");
    }), this);

    gtk_box_append(GTK_BOX(controls), previous_button_);
    gtk_box_append(GTK_BOX(controls), play_pause_button_);
    gtk_box_append(GTK_BOX(controls), next_button_);
    gtk_box_append(GTK_BOX(root), album_stack_);
    gtk_box_append(GTK_BOX(root), title_label_);
    gtk_box_append(GTK_BOX(root), artist_label_);
    gtk_box_append(GTK_BOX(root), controls);
    gtk_popover_set_child(GTK_POPOVER(popover_), root);

    update(std::nullopt);
}

MediaWidget::~MediaWidget() {
    async_state_->alive = false;
    ++async_state_->art_generation;
    async_state_->owner = nullptr;
    if (popover_ != nullptr && gtk_widget_get_parent(popover_) != nullptr) {
        gtk_widget_unparent(popover_);
    }
}

void MediaWidget::toggle() {
    if (gtk_widget_get_visible(popover_)) {
        gtk_popover_popdown(GTK_POPOVER(popover_));
        return;
    }
    if (request_exclusive_open_) request_exclusive_open_(GTK_POPOVER(popover_));
    gtk_popover_popup(GTK_POPOVER(popover_));
}

void MediaWidget::close() {
    if (popover_ != nullptr) gtk_popover_popdown(GTK_POPOVER(popover_));
}

void MediaWidget::invoke_control(const char* method) {
    auto* service = &media_service_;
    const std::string action(method != nullptr ? method : "");
    static_cast<void>(realmheart::core::shared_task_executor().post([service, action] {
        if (action == "previous") static_cast<void>(service->previous());
        else if (action == "next") static_cast<void>(service->next());
        else static_cast<void>(service->play_pause());
    }));
    gtk_popover_popdown(GTK_POPOVER(popover_));
}

void MediaWidget::update_art(const std::string& art_url) {
    if (art_url == requested_art_url_ && art_request_complete_) return;

    requested_art_url_ = art_url;
    art_request_complete_ = art_url.empty();
    const std::uint64_t generation = ++async_state_->art_generation;
    gtk_picture_set_paintable(GTK_PICTURE(album_picture_), nullptr);
    gtk_stack_set_visible_child_name(GTK_STACK(album_stack_), "fallback");
    if (art_url.empty()) return;

    const auto state = async_state_;
    const std::string requested_url = art_url;
    const bool posted = realmheart::core::shared_task_executor().post(
        [state, requested_url, generation] {
            const auto cancelled = [state, generation] {
                return !state->alive.load() || state->art_generation.load() != generation;
            };
            const auto path = MediaArtLoader::resolve(requested_url, cancelled);

            GdkPixbuf* pixbuf = nullptr;
            if (path && !cancelled()) {
                GError* error = nullptr;
                pixbuf = gdk_pixbuf_new_from_file_at_scale(
                    path->string().c_str(), 204, 116, TRUE, &error
                );
                if (pixbuf == nullptr) g_clear_error(&error);
            }

            struct Payload {
                std::shared_ptr<AsyncState> state;
                std::string art_url;
                std::uint64_t generation = 0;
                GdkPixbuf* pixbuf = nullptr;

                ~Payload() {
                    if (pixbuf != nullptr) g_object_unref(pixbuf);
                }
            };

            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<Payload*>(raw);
                    if (payload->state->alive.load() &&
                        payload->state->art_generation.load() == payload->generation &&
                        payload->state->owner != nullptr) {
                        payload->state->owner->apply_art(
                            payload->art_url, payload->generation, payload->pixbuf
                        );
                    }
                    return G_SOURCE_REMOVE;
                },
                new Payload{state, requested_url, generation, pixbuf},
                +[](gpointer raw) { delete static_cast<Payload*>(raw); }
            );
        }
    );

    if (!posted) art_request_complete_ = true;
}

void MediaWidget::apply_art(
    const std::string& art_url,
    std::uint64_t generation,
    GdkPixbuf* pixbuf
) {
    if (generation != async_state_->art_generation.load() || art_url != requested_art_url_) return;
    art_request_complete_ = true;
    if (pixbuf == nullptr) {
        gtk_stack_set_visible_child_name(GTK_STACK(album_stack_), "fallback");
        return;
    }

    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int source_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const auto* source_pixels = gdk_pixbuf_read_pixels(pixbuf);

    if (width <= 0 || height <= 0 || source_pixels == nullptr ||
        (channels != 3 && channels != 4)) {
        gtk_stack_set_visible_child_name(GTK_STACK(album_stack_), "fallback");
        return;
    }

    const gsize texture_stride = static_cast<gsize>(width) * static_cast<gsize>(channels);
    const gsize texture_size = texture_stride * static_cast<gsize>(height);
    auto* texture_pixels = static_cast<guint8*>(g_malloc(texture_size));

    for (int row = 0; row < height; ++row) {
        std::memcpy(
            texture_pixels + static_cast<gsize>(row) * texture_stride,
            source_pixels + static_cast<gsize>(row) * static_cast<gsize>(source_stride),
            texture_stride
        );
    }

    GBytes* bytes = g_bytes_new_take(texture_pixels, texture_size);
    const GdkMemoryFormat format = channels == 4
        ? GDK_MEMORY_R8G8B8A8
        : GDK_MEMORY_R8G8B8;
    GdkTexture* texture = gdk_memory_texture_new(
        width, height, format, bytes, texture_stride
    );
    g_bytes_unref(bytes);

    gtk_picture_set_paintable(GTK_PICTURE(album_picture_), GDK_PAINTABLE(texture));
    g_object_unref(texture);
    gtk_stack_set_visible_child_name(GTK_STACK(album_stack_), "picture");
}

void MediaWidget::update(const std::optional<services::MediaInfo>& info) {
    info_ = info;
    const bool available = info_.has_value();
    const bool playing = available && info_->playback_status == 1;

    button_.set_enabled(available);

    if (!available) {
        button_.set_tooltip("No active media player");
        gtk_label_set_text(GTK_LABEL(title_label_), "No active media");
        gtk_label_set_text(GTK_LABEL(artist_label_), "");
        update_art({});
    } else {
        const std::string title = info_->title.empty() ? "Untitled track" : info_->title;
        const std::string artist = info_->artist.empty() ? info_->album : info_->artist;
        button_.set_tooltip("Media: " + artist + " — " + title);
        gtk_label_set_text(GTK_LABEL(title_label_), title.c_str());
        gtk_label_set_text(GTK_LABEL(artist_label_), artist.c_str());
        update_art(info_->art_url);
    }

    gtk_widget_set_sensitive(previous_button_, available);
    gtk_widget_set_sensitive(play_pause_button_, available);
    gtk_widget_set_sensitive(next_button_, available);
    static_cast<void>(play_pause_icon_->set_icon(
        playing ? "Realmheart-Icons/pause.svg" : "Realmheart-Icons/play.svg"
    ));
}

} // namespace realmheart::ui::bar::widgets
