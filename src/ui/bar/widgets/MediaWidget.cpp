#include "ui/bar/widgets/MediaWidget.hpp"

#include "ui/bar/widgets/AttachedPopover.hpp"
#include "ui/LayerSurface.hpp"
#include "core/TaskExecutor.hpp"
#include "ui/bar/MediaArtLoader.hpp"
#include "ui/bar/widgets/SlideClip.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

// Preserve the final two-pixel horizontal tune from the popover version. The
// layer surface computes its base position from the media button's real bounds.
constexpr int kMediaLayerExtraOffsetX = kExpandingPopoverOffsetX + 2;
// The media surface itself becomes the outside-click catcher. Its Wayland
// input region excludes the physical taskbar rail except where the attached
// media shell overlaps it, so bar controls still receive their normal clicks.
// The media surface now owns the physical top edge, so the complete screen-hug
// shoulder can use the same depth as the lower taskbar shoulder.
constexpr int kMediaTopCurveHeight = 22;
// The full shell keeps its final allocation while a snapshot clip advances
// from left to right. This preserves every Cairo curve throughout the reveal
// and gives concealment the exact reverse motion back into the taskbar.
constexpr guint kMediaRevealDurationMs = 240;
// The media shell has a decorative screen-hug shoulder on its far-right edge.
// Reveal from the attached interior instead of translating that shoulder into
// view first. The content host begins at x=14 and the centred album artwork
// begins at x=22. Stopping the final conceal sample at x=14 keeps the last
// clipped column inside the clean panel gutter instead of slicing through the
// album image and producing a false gold/brown vertical streak.
constexpr guint kMediaRevealTravelPx = 14;
constexpr int kAlbumArtWidth = 204;
constexpr int kAlbumArtHeight = 92;
constexpr int kAlbumArtLoadSize = 512;
constexpr int kMediaTopSpacerHeight = 14;
constexpr int kMediaBottomSpacerHeight = 8;
constexpr guint kSeekCommitDelayMs = 180;
constexpr guint kPositionRefreshIntervalMs = 500;

std::string media_time_text(std::int64_t microseconds) {
    const std::int64_t total_seconds = std::max<std::int64_t>(0, microseconds / 1'000'000);
    const std::int64_t hours = total_seconds / 3600;
    const std::int64_t minutes = (total_seconds % 3600) / 60;
    const std::int64_t seconds = total_seconds % 60;

    char buffer[32]{};
    if (hours > 0) {
        std::snprintf(
            buffer, sizeof(buffer), "%lld:%02lld:%02lld",
            static_cast<long long>(hours),
            static_cast<long long>(minutes),
            static_cast<long long>(seconds)
        );
    } else {
        std::snprintf(
            buffer, sizeof(buffer), "%lld:%02lld",
            static_cast<long long>(minutes),
            static_cast<long long>(seconds)
        );
    }
    return buffer;
}

void rounded_rectangle(cairo_t* cr, double x, double y, double width, double height, double radius) {
    const double right = x + width;
    const double bottom = y + height;
    cairo_new_sub_path(cr);
    cairo_arc(cr, right - radius, y + radius, radius, -G_PI_2, 0.0);
    cairo_arc(cr, right - radius, bottom - radius, radius, 0.0, G_PI_2);
    cairo_arc(cr, x + radius, bottom - radius, radius, G_PI_2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3.0 * G_PI_2);
    cairo_close_path(cr);
}

GdkPixbuf* crop_album_art_banner(GdkPixbuf* source) {
    if (source == nullptr) return nullptr;

    const int source_width = gdk_pixbuf_get_width(source);
    const int source_height = gdk_pixbuf_get_height(source);
    if (source_width <= 0 || source_height <= 0) return nullptr;

    constexpr double target_ratio =
        static_cast<double>(kAlbumArtWidth) / static_cast<double>(kAlbumArtHeight);
    const double source_ratio =
        static_cast<double>(source_width) / static_cast<double>(source_height);

    int crop_width = source_width;
    int crop_height = source_height;
    if (source_ratio > target_ratio) {
        crop_width = std::clamp(
            static_cast<int>(std::lround(static_cast<double>(source_height) * target_ratio)),
            1,
            source_width
        );
    } else {
        crop_height = std::clamp(
            static_cast<int>(std::lround(static_cast<double>(source_width) / target_ratio)),
            1,
            source_height
        );
    }

    const int crop_x = (source_width - crop_width) / 2;
    const int crop_y = (source_height - crop_height) / 2;
    GdkPixbuf* cropped = gdk_pixbuf_new_subpixbuf(
        source, crop_x, crop_y, crop_width, crop_height
    );
    if (cropped == nullptr) return nullptr;

    GdkPixbuf* banner = gdk_pixbuf_scale_simple(
        cropped,
        kAlbumArtWidth,
        kAlbumArtHeight,
        GDK_INTERP_HYPER
    );
    g_object_unref(cropped);
    return banner;
}

void draw_album_art_shadow(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer
) {
    if (width <= 0 || height <= 0) return;

    constexpr double radius = 12.0;

    // GtkOverlay overflow clipping is rectangular. Paint the four outside
    // corners with the panel colour so the artwork is visibly rounded even on
    // GTK renderers that do not clip child content to the CSS border radius.
    GdkRGBA panel_fill{};
    gtk_widget_get_color(GTK_WIDGET(area), &panel_fill);
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_rectangle(cr, 0.0, 0.0, width, height);
    rounded_rectangle(cr, 0.0, 0.0, width, height, radius);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    gdk_cairo_set_source_rgba(cr, &panel_fill);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_save(cr);
    rounded_rectangle(cr, 0.0, 0.0, width, height, radius);
    cairo_clip(cr);

    cairo_pattern_t* vignette = cairo_pattern_create_linear(0.0, 0.0, 0.0, height);
    cairo_pattern_add_color_stop_rgba(vignette, 0.0, 0.0, 0.0, 0.0, 0.22);
    cairo_pattern_add_color_stop_rgba(vignette, 0.24, 0.0, 0.0, 0.0, 0.04);
    cairo_pattern_add_color_stop_rgba(vignette, 0.62, 0.0, 0.0, 0.0, 0.00);
    cairo_pattern_add_color_stop_rgba(vignette, 1.0, 0.0, 0.0, 0.0, 0.30);
    cairo_rectangle(cr, 0.0, 0.0, width, height);
    cairo_set_source(cr, vignette);
    cairo_fill(cr);
    cairo_pattern_destroy(vignette);

    rounded_rectangle(cr, 0.75, 0.75, width - 1.5, height - 1.5, radius - 0.75);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.32);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    cairo_restore(cr);
}

} // namespace

MediaWidget::MediaWidget(
    GtkApplication* app,
    services::MediaService& media_service,
    std::function<void()> request_exclusive_open,
    std::function<void(int)> set_bar_contour_occlusion,
    int monitor_index
) : media_service_(media_service),
    monitor_index_(monitor_index),
    request_exclusive_open_(std::move(request_exclusive_open)),
    set_bar_contour_occlusion_(std::move(set_bar_contour_occlusion)),
    button_(
        "Realmheart-Icons/media.svg",
        "Md",
        "Media",
        [this] { toggle(); }
    ) {
    async_state_->owner = this;
    button_.add_css_class("realmheart-media-button");

    // The visible media panel is a real layer surface anchored to the physical
    // monitor edge. Hyprland's tiled-window gaps and GDK popup constraints no
    // longer participate in its vertical placement.
    layer_window_ = app != nullptr
        ? gtk_application_window_new(app)
        : gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(layer_window_), "Realmheart Media");
    gtk_window_set_decorated(GTK_WINDOW(layer_window_), FALSE);
    // The media window now spans the output and contains both the transparent
    // outside-click target and the fixed-position media shell. The visible
    // panel keeps its original natural size inside this fullscreen surface.
    gtk_window_set_resizable(GTK_WINDOW(layer_window_), TRUE);
    gtk_widget_add_css_class(layer_window_, "realmheart-media-layer-window");
    gtk_widget_remove_css_class(layer_window_, "background");

    realmheart::ui::LayerSurfaceSpec media_surface;
    media_surface.surface_namespace = "realmheart-media";
    media_surface.layer = realmheart::ui::LayerSurfaceLevel::Overlay;
    media_surface.keyboard_mode = realmheart::ui::LayerKeyboardMode::None;
    media_surface.anchor_left = true;
    media_surface.anchor_right = true;
    media_surface.anchor_top = true;
    media_surface.anchor_bottom = true;
    media_surface.exclusive_zone = 0;
    media_surface.monitor_index = monitor_index_;
    realmheart::ui::apply_layer_surface(GTK_WINDOW(layer_window_), media_surface);

    // The vertical bar already reserves a left-side exclusive zone. A normal
    // zero-zone layer surface is positioned inside that usable area, so adding
    // the media button's physical X coordinate would count the bar width twice.
    // -1 deliberately ignores existing exclusive zones; our explicit left
    // margin can then align the media shell against the real monitor origin.
    gtk_layer_set_exclusive_zone(GTK_WINDOW(layer_window_), -1);

    // Keep dismissal and the interactive panel inside one Wayland surface. A
    // separate transparent layer was compositor-order dependent and never
    // reliably received presses on this setup. GtkOverlay gives us a guaranteed
    // full-output background target with the media shell as a sibling above it.
    layer_overlay_ = gtk_overlay_new();
    gtk_widget_set_hexpand(layer_overlay_, TRUE);
    gtk_widget_set_vexpand(layer_overlay_, TRUE);

    dismiss_target_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(dismiss_target_, TRUE);
    gtk_widget_set_vexpand(dismiss_target_, TRUE);
    gtk_widget_set_can_target(dismiss_target_, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(layer_overlay_), dismiss_target_);

    GtkGesture* dismiss_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(
        GTK_GESTURE_SINGLE(dismiss_click),
        GDK_BUTTON_PRIMARY
    );
    g_signal_connect(
        dismiss_click,
        "pressed",
        G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer data) {
            static_cast<MediaWidget*>(data)->close();
        }),
        this
    );
    gtk_widget_add_controller(
        dismiss_target_,
        GTK_EVENT_CONTROLLER(dismiss_click)
    );

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(root, "realmheart-media-content");
    gtk_widget_set_size_request(root, 220, -1);

    // Real measured children are used instead of margins here. GtkOverlay does
    // not reliably include overlay-child margins in its requested size, which
    // is why the previous media-only padding changed source code but not the
    // visible shell height.
    GtkWidget* top_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(top_spacer, -1, kMediaTopSpacerHeight);
    gtk_widget_set_can_target(top_spacer, FALSE);

    GtkWidget* bottom_spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(bottom_spacer, -1, kMediaBottomSpacerHeight);
    gtk_widget_set_can_target(bottom_spacer, FALSE);

    GtkWidget* album_frame = gtk_overlay_new();
    gtk_widget_add_css_class(album_frame, "realmheart-media-art-frame");
    gtk_widget_set_size_request(album_frame, kAlbumArtWidth, kAlbumArtHeight);
    gtk_widget_set_halign(album_frame, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(album_frame, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(album_frame, FALSE);
    gtk_widget_set_vexpand(album_frame, FALSE);
    gtk_widget_set_overflow(album_frame, GTK_OVERFLOW_HIDDEN);

    album_stack_ = gtk_stack_new();
    gtk_widget_add_css_class(album_stack_, "realmheart-media-art");
    gtk_widget_set_size_request(album_stack_, kAlbumArtWidth, kAlbumArtHeight);
    gtk_widget_set_hexpand(album_stack_, TRUE);
    gtk_widget_set_vexpand(album_stack_, TRUE);
    album_picture_ = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(album_picture_), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(album_picture_), TRUE);
    gtk_picture_set_alternative_text(GTK_PICTURE(album_picture_), "Album artwork");
    gtk_widget_set_size_request(album_picture_, kAlbumArtWidth, kAlbumArtHeight);
    gtk_widget_set_hexpand(album_picture_, TRUE);
    gtk_widget_set_vexpand(album_picture_, TRUE);
    album_fallback_ = gtk_label_new("No album art");
    gtk_widget_add_css_class(album_fallback_, "realmheart-media-art-fallback");
    gtk_stack_add_named(GTK_STACK(album_stack_), album_picture_, "picture");
    gtk_stack_add_named(GTK_STACK(album_stack_), album_fallback_, "fallback");
    gtk_stack_set_visible_child_name(GTK_STACK(album_stack_), "fallback");
    gtk_overlay_set_child(GTK_OVERLAY(album_frame), album_stack_);

    GtkWidget* album_shadow = gtk_drawing_area_new();
    gtk_widget_add_css_class(album_shadow, "realmheart-media-art-overlay");
    gtk_widget_set_can_target(album_shadow, FALSE);
    gtk_widget_set_hexpand(album_shadow, TRUE);
    gtk_widget_set_vexpand(album_shadow, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(album_shadow),
        draw_album_art_shadow,
        nullptr,
        nullptr
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(album_frame), album_shadow);

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

    GtkWidget* seek_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_add_css_class(seek_row, "realmheart-media-seek-row");
    gtk_widget_set_hexpand(seek_row, TRUE);

    position_label_ = gtk_label_new("0:00");
    gtk_widget_add_css_class(position_label_, "realmheart-media-time");
    gtk_label_set_xalign(GTK_LABEL(position_label_), 0.0F);

    seek_scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 1.0);
    gtk_widget_add_css_class(seek_scale_, "realmheart-media-seek");
    gtk_widget_set_hexpand(seek_scale_, TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(seek_scale_), FALSE);
    gtk_range_set_increments(GTK_RANGE(seek_scale_), 5.0, 15.0);
    gtk_widget_set_sensitive(seek_scale_, FALSE);

    duration_label_ = gtk_label_new("0:00");
    gtk_widget_add_css_class(duration_label_, "realmheart-media-time");
    gtk_label_set_xalign(GTK_LABEL(duration_label_), 1.0F);

    gtk_box_append(GTK_BOX(seek_row), position_label_);
    gtk_box_append(GTK_BOX(seek_row), seek_scale_);
    gtk_box_append(GTK_BOX(seek_row), duration_label_);

    // value-changed works for trough clicks, keyboard input, and dragging.
    // A guard prevents programmatic position refreshes from scheduling seeks.
    g_signal_connect(
        seek_scale_,
        "value-changed",
        G_CALLBACK(+[](GtkRange* range, gpointer data) {
            auto* self = static_cast<MediaWidget*>(data);
            if (self->updating_seek_ui_) return;
            self->schedule_seek(gtk_range_get_value(range));
        }),
        this
    );


    // GtkScale normally handles both trough clicks and handle dragging, but
    // some GTK themes/controllers only page by the adjustment increment. This
    // controller guarantees that a direct click maps to the exact song fraction.
    GtkGesture* seek_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(seek_click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(seek_click), GTK_PHASE_CAPTURE
    );
    g_signal_connect(
        seek_click,
        "pressed",
        G_CALLBACK(+[](GtkGestureClick*, int, double x, double, gpointer data) {
            auto* self = static_cast<MediaWidget*>(data);
            if (self->seek_scale_ == nullptr ||
                !gtk_widget_get_sensitive(self->seek_scale_)) return;

            const int width = gtk_widget_get_width(self->seek_scale_);
            if (width <= 1) return;
            GtkAdjustment* adjustment = gtk_range_get_adjustment(GTK_RANGE(self->seek_scale_));
            const double lower = gtk_adjustment_get_lower(adjustment);
            const double upper = gtk_adjustment_get_upper(adjustment);
            const double fraction = std::clamp(x / static_cast<double>(width), 0.0, 1.0);
            gtk_range_set_value(
                GTK_RANGE(self->seek_scale_),
                lower + ((upper - lower) * fraction)
            );
        }),
        this
    );
    gtk_widget_add_controller(seek_scale_, GTK_EVENT_CONTROLLER(seek_click));

    GtkWidget* controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(controls, "realmheart-media-controls");
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
    gtk_box_append(GTK_BOX(root), top_spacer);
    gtk_box_append(GTK_BOX(root), album_frame);
    gtk_box_append(GTK_BOX(root), title_label_);
    gtk_box_append(GTK_BOX(root), artist_label_);
    gtk_box_append(GTK_BOX(root), seek_row);
    gtk_box_append(GTK_BOX(root), controls);
    gtk_box_append(GTK_BOX(root), bottom_spacer);
    GtkWidget* media_shell = create_expanding_popover_shell(
        root,
        kMediaTopCurveHeight,
        true,
        true
    );

    // GtkRevealer changes the child's allocation during a slide, which made
    // the Cairo shell redraw itself as a temporary 10-20 px-wide panel. The
    // custom clip keeps media_shell at its complete final width on every frame
    // and reveals only its snapshot from the taskbar edge toward the right.
    layer_clip_ = realmheart_slide_clip_new(
        media_shell,
        kMediaRevealDurationMs
    );
    realmheart_slide_clip_set_leading_edge_reveal(
        REALMHEART_SLIDE_CLIP(layer_clip_),
        kMediaRevealTravelPx
    );
    g_signal_connect(
        layer_clip_,
        "concealed",
        G_CALLBACK(+[](RealmheartSlideClip*, gpointer data) {
            auto* self = static_cast<MediaWidget*>(data);
            if (!self->layer_open_ && self->layer_window_ != nullptr) {
                gtk_widget_set_visible(self->layer_window_, FALSE);
                // The taskbar and media panel are separate Wayland surfaces.
                // Restoring the bar contour in this same frame can commit one
                // frame before the media surface's unmap, which produces the
                // hairline gold streak seen at the end of concealment. Keep the
                // contour suppressed until the next taskbar frame instead.
                self->schedule_bar_contour_restore();
            }
        }),
        this
    );
    gtk_widget_set_halign(layer_clip_, GTK_ALIGN_START);
    gtk_widget_set_valign(layer_clip_, GTK_ALIGN_START);
    gtk_widget_set_hexpand(layer_clip_, FALSE);
    gtk_widget_set_vexpand(layer_clip_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(layer_overlay_), layer_clip_);
    gtk_overlay_set_measure_overlay(
        GTK_OVERLAY(layer_overlay_),
        layer_clip_,
        FALSE
    );
    gtk_window_set_child(GTK_WINDOW(layer_window_), layer_overlay_);

    update(std::nullopt);
}

MediaWidget::~MediaWidget() {
    hide_layer_window();
    cancel_bar_contour_restore();
    if (set_bar_contour_occlusion_) set_bar_contour_occlusion_(0);
    if (seek_commit_timer_id_ != 0) {
        g_source_remove(seek_commit_timer_id_);
        seek_commit_timer_id_ = 0;
    }
    async_state_->alive = false;
    ++async_state_->art_generation;
    async_state_->owner = nullptr;
    if (layer_window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(layer_window_));
        layer_window_ = nullptr;
    }
}

void MediaWidget::set_layout(const BarGeometry& geometry) {
    button_.set_layout(geometry.icon_button_extent, geometry.icon_size);
    taskbar_input_pass_through_width_ = geometry.rail_width;
    if (layer_open_) update_layer_input_region();
}

void MediaWidget::cancel_bar_contour_restore() {
    if (contour_restore_tick_id_ == 0) return;

    GtkWidget* clock_widget = button_.widget();
    if (clock_widget != nullptr) {
        gtk_widget_remove_tick_callback(clock_widget, contour_restore_tick_id_);
    }
    contour_restore_tick_id_ = 0;
}

void MediaWidget::schedule_bar_contour_restore() {
    cancel_bar_contour_restore();
    if (!set_bar_contour_occlusion_) return;

    GtkWidget* clock_widget = button_.widget();
    if (clock_widget == nullptr) {
        set_bar_contour_occlusion_(0);
        return;
    }

    contour_restore_tick_id_ = gtk_widget_add_tick_callback(
        clock_widget,
        +[](GtkWidget*, GdkFrameClock*, gpointer data) -> gboolean {
            auto* self = static_cast<MediaWidget*>(data);
            self->contour_restore_tick_id_ = 0;

            // A reopen may happen before this frame arrives. In that case the
            // media shell still owns this section of the contour, so leave the
            // taskbar's underlying stroke suppressed.
            if (!self->layer_open_ &&
                (self->layer_window_ == nullptr ||
                 !gtk_widget_get_visible(self->layer_window_)) &&
                self->set_bar_contour_occlusion_) {
                self->set_bar_contour_occlusion_(0);
            }
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

int MediaWidget::layer_left_margin() const {
    GtkWidget* button = button_.button();
    GtkWidget* bar_window = button != nullptr
        ? gtk_widget_get_ancestor(button, GTK_TYPE_WINDOW)
        : nullptr;
    if (button == nullptr || bar_window == nullptr) return taskbar_input_pass_through_width_;

    graphene_rect_t bounds{};
    if (!gtk_widget_compute_bounds(button, bar_window, &bounds)) {
        return taskbar_input_pass_through_width_;
    }

    return std::max(
        0,
        static_cast<int>(std::lround(bounds.origin.x + bounds.size.width))
            + kMediaLayerExtraOffsetX
    );
}

void MediaWidget::update_layer_input_region() {
    if (layer_window_ == nullptr || layer_clip_ == nullptr) return;

    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(layer_window_));
    if (surface == nullptr) return;

    const int width = gdk_surface_get_width(surface);
    const int height = gdk_surface_get_height(surface);
    if (width <= 0 || height <= 0) return;

    cairo_region_t* input_region = cairo_region_create();

    // Everything to the right of the taskbar is an outside-click target.
    if (width > taskbar_input_pass_through_width_) {
        const cairo_rectangle_int_t outside_region{
            taskbar_input_pass_through_width_,
            0,
            width - taskbar_input_pass_through_width_,
            height
        };
        cairo_region_union_rectangle(input_region, &outside_region);
    }

    // The attached media shell begins slightly inside the rail. Include its
    // exact rectangle so its border and controls remain interactive while the
    // rest of the taskbar stays pointer-pass-through.
    const int media_width = gtk_widget_get_width(layer_clip_);
    const int media_height = gtk_widget_get_height(layer_clip_);
    if (media_width > 0 && media_height > 0) {
        const cairo_rectangle_int_t media_region{
            gtk_widget_get_margin_start(layer_clip_),
            gtk_widget_get_margin_top(layer_clip_),
            media_width,
            media_height
        };
        cairo_region_union_rectangle(input_region, &media_region);
    }

    gdk_surface_set_input_region(surface, input_region);
    cairo_region_destroy(input_region);
}

void MediaWidget::show_layer_window() {
    if (layer_window_ == nullptr || layer_clip_ == nullptr) return;

    cancel_bar_contour_restore();
    layer_open_ = true;

    // The fullscreen surface stays fixed at the monitor origin. Move only the
    // naturally sized media child to the exact finalized resting coordinate.
    gtk_widget_set_margin_start(layer_clip_, layer_left_margin());
    gtk_widget_set_margin_top(layer_clip_, 0);
    gtk_widget_set_opacity(layer_window_, 1.0);

    // Suppress the bar's own contour beneath the media shell before presenting
    // the overlay surface. Otherwise that stationary line is progressively
    // uncovered by the moving lower shoulder during concealment. Measurement
    // works before mapping; the first mapped tick below refreshes the exact
    // allocated height once GTK has completed layout.
    if (set_bar_contour_occlusion_) {
        int minimum_height = 0;
        int natural_height = 0;
        gtk_widget_measure(
            layer_clip_,
            GTK_ORIENTATION_VERTICAL,
            -1,
            &minimum_height,
            &natural_height,
            nullptr,
            nullptr
        );
        set_bar_contour_occlusion_(std::max(minimum_height, natural_height));
    }

    if (gtk_widget_get_visible(layer_window_)) {
        // The dismiss surface may have been remapped during a mid-concealment
        // reversal. Re-present media so it remains stacked above the catcher.
        gtk_window_present(GTK_WINDOW(layer_window_));
        update_layer_input_region();
        // Reopening during concealment retargets the clip from its exact current
        // progress, so the animation reverses without a snap.
        realmheart_slide_clip_set_revealed(
            REALMHEART_SLIDE_CLIP(layer_clip_),
            TRUE
        );
        return;
    }

    if (reveal_start_tick_id_ != 0) {
        gtk_widget_remove_tick_callback(layer_window_, reveal_start_tick_id_);
        reveal_start_tick_id_ = 0;
    }

    realmheart_slide_clip_set_revealed_immediately(
        REALMHEART_SLIDE_CLIP(layer_clip_),
        FALSE
    );
    gtk_window_present(GTK_WINDOW(layer_window_));

    // Map one fully concealed frame first. Beginning on the next compositor
    // frame prevents the window's initial map from skipping the clipped start.
    reveal_start_tick_id_ = gtk_widget_add_tick_callback(
        layer_window_,
        +[](GtkWidget* widget, GdkFrameClock*, gpointer data) -> gboolean {
            auto* self = static_cast<MediaWidget*>(data);
            self->reveal_start_tick_id_ = 0;
            if (self->layer_open_ && gtk_widget_get_visible(widget) &&
                self->layer_clip_ != nullptr) {
                self->update_layer_input_region();
                if (self->set_bar_contour_occlusion_) {
                    self->set_bar_contour_occlusion_(
                        gtk_widget_get_height(self->layer_clip_)
                    );
                }
                realmheart_slide_clip_set_revealed(
                    REALMHEART_SLIDE_CLIP(self->layer_clip_),
                    TRUE
                );
            }
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

void MediaWidget::hide_layer_window() {
    layer_open_ = false;
    stop_position_refresh();

    if (layer_window_ == nullptr || layer_clip_ == nullptr) return;

    if (reveal_start_tick_id_ != 0) {
        gtk_widget_remove_tick_callback(layer_window_, reveal_start_tick_id_);
        reveal_start_tick_id_ = 0;
    }

    if (!gtk_widget_get_visible(layer_window_)) {
        realmheart_slide_clip_set_revealed_immediately(
            REALMHEART_SLIDE_CLIP(layer_clip_),
            FALSE
        );
        cancel_bar_contour_restore();
        if (set_bar_contour_occlusion_) set_bar_contour_occlusion_(0);
        return;
    }

    realmheart_slide_clip_set_revealed(
        REALMHEART_SLIDE_CLIP(layer_clip_),
        FALSE
    );

    if (realmheart_slide_clip_is_concealed(
            REALMHEART_SLIDE_CLIP(layer_clip_)
        )) {
        gtk_widget_set_visible(layer_window_, FALSE);
        schedule_bar_contour_restore();
    }
}

void MediaWidget::toggle() {
    if (layer_open_) {
        close();
        return;
    }
    if (request_exclusive_open_) request_exclusive_open_();
    show_layer_window();
    start_position_refresh();
}

void MediaWidget::close() {
    hide_layer_window();
}

void MediaWidget::invoke_control(const char* method) {
    auto* service = &media_service_;
    const std::string action(method != nullptr ? method : "");
    static_cast<void>(realmheart::core::shared_task_executor().post([service, action] {
        if (action == "previous") static_cast<void>(service->previous());
        else if (action == "next") static_cast<void>(service->next());
        else static_cast<void>(service->play_pause());
    }));
}

void MediaWidget::start_position_refresh() {
    refresh_seek_ui();
    if (position_refresh_timer_id_ != 0) return;

    position_refresh_timer_id_ = g_timeout_add(
        kPositionRefreshIntervalMs,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<MediaWidget*>(data);
            if (self->layer_window_ == nullptr ||
                !gtk_widget_get_visible(self->layer_window_)) {
                self->position_refresh_timer_id_ = 0;
                return G_SOURCE_REMOVE;
            }
            self->refresh_seek_ui();
            return G_SOURCE_CONTINUE;
        },
        this
    );
}

void MediaWidget::stop_position_refresh() {
    if (position_refresh_timer_id_ != 0) {
        g_source_remove(position_refresh_timer_id_);
        position_refresh_timer_id_ = 0;
    }
}

std::int64_t MediaWidget::estimated_position_us() const {
    if (!info_) return 0;

    std::int64_t position = std::max<std::int64_t>(0, position_anchor_us_);
    if (info_->playback_status == 1 && position_anchor_monotonic_us_ > 0) {
        position += std::max<gint64>(0, g_get_monotonic_time() - position_anchor_monotonic_us_);
    }
    if (info_->length_us > 0) position = std::min(position, info_->length_us);
    return position;
}

void MediaWidget::refresh_seek_ui() {
    const bool seekable = info_.has_value() && info_->length_us > 0;
    const std::int64_t duration_us = info_ ? std::max<std::int64_t>(0, info_->length_us) : 0;
    const std::int64_t position_us = has_pending_seek_
        ? static_cast<std::int64_t>(pending_seek_seconds_ * 1'000'000.0)
        : estimated_position_us();

    updating_seek_ui_ = true;
    gtk_widget_set_sensitive(seek_scale_, seekable);
    gtk_widget_set_tooltip_text(
        seek_scale_,
        seekable
            ? "Click or drag to seek"
            : "This player did not report the track duration"
    );
    gtk_range_set_range(
        GTK_RANGE(seek_scale_),
        0.0,
        std::max(1.0, static_cast<double>(duration_us) / 1'000'000.0)
    );
    if (!has_pending_seek_) {
        gtk_range_set_value(
            GTK_RANGE(seek_scale_),
            static_cast<double>(position_us) / 1'000'000.0
        );
    }
    updating_seek_ui_ = false;

    const std::string position_text = media_time_text(position_us);
    const std::string duration_text = media_time_text(duration_us);
    gtk_label_set_text(GTK_LABEL(position_label_), position_text.c_str());
    gtk_label_set_text(GTK_LABEL(duration_label_), duration_text.c_str());
}

void MediaWidget::schedule_seek(double seconds) {
    if (!info_ || info_->length_us <= 0) return;

    const double duration_seconds = static_cast<double>(info_->length_us) / 1'000'000.0;
    pending_seek_seconds_ = std::clamp(seconds, 0.0, duration_seconds);
    has_pending_seek_ = true;

    const std::string preview = media_time_text(
        static_cast<std::int64_t>(pending_seek_seconds_ * 1'000'000.0)
    );
    gtk_label_set_text(GTK_LABEL(position_label_), preview.c_str());

    if (seek_commit_timer_id_ != 0) g_source_remove(seek_commit_timer_id_);
    seek_commit_timer_id_ = g_timeout_add(
        kSeekCommitDelayMs,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<MediaWidget*>(data);
            self->seek_commit_timer_id_ = 0;
            self->commit_pending_seek();
            return G_SOURCE_REMOVE;
        },
        this
    );
}

void MediaWidget::commit_pending_seek() {
    if (!has_pending_seek_ || !info_ || info_->length_us <= 0) {
        has_pending_seek_ = false;
        return;
    }

    if (seek_commit_timer_id_ != 0) {
        g_source_remove(seek_commit_timer_id_);
        seek_commit_timer_id_ = 0;
    }

    const std::int64_t current_us = estimated_position_us();
    const std::int64_t target_us = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(pending_seek_seconds_ * 1'000'000.0),
        0,
        info_->length_us
    );
    const std::string player_bus_name = info_->player_bus_name;
    const std::string track_id = info_->track_id;
    has_pending_seek_ = false;
    position_anchor_us_ = target_us;
    position_anchor_monotonic_us_ = g_get_monotonic_time();
    info_->position_us = target_us;
    refresh_seek_ui();

    auto* service = &media_service_;
    static_cast<void>(realmheart::core::shared_task_executor().post(
        [service, player_bus_name, track_id, current_us, target_us] {
            static_cast<void>(service->seek_to(
                player_bus_name, track_id, current_us, target_us
            ));
        }
    ));
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
                GdkPixbuf* source = gdk_pixbuf_new_from_file_at_scale(
                    path->string().c_str(),
                    kAlbumArtLoadSize,
                    kAlbumArtLoadSize,
                    TRUE,
                    &error
                );
                if (source == nullptr) {
                    g_clear_error(&error);
                } else {
                    pixbuf = crop_album_art_banner(source);
                    g_object_unref(source);
                }
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
    if (!has_pending_seek_) {
        position_anchor_us_ = info_ ? std::max<std::int64_t>(0, info_->position_us) : 0;
        position_anchor_monotonic_us_ = g_get_monotonic_time();
    }
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
    refresh_seek_ui();
}

} // namespace realmheart::ui::bar::widgets
