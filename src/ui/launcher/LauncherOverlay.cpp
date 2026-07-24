#include "ui/launcher/LauncherOverlay.hpp"

#include "ui/LayerSurface.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace realmheart::ui {
namespace {

constexpr int kRecommendationCount = 4;
constexpr int kResultCount = 8;

void clear_box(GtkWidget* box) {
    GtkWidget* child = gtk_widget_get_first_child(box);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

void clear_list(GtkWidget* list) {
    GtkWidget* child = gtk_widget_get_first_child(list);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(list), child);
        child = next;
    }
}

const char* result_kind_label(services::LauncherResultKind kind) {
    switch (kind) {
    case services::LauncherResultKind::Application:
        return "APPLICATION";
    case services::LauncherResultKind::Command:
        return "COMMAND";
    case services::LauncherResultKind::Action:
        return "REALMHEART ACTION";
    case services::LauncherResultKind::Emoji:
        return "EMOJI";
    case services::LauncherResultKind::Clipboard:
        return "CLIPBOARD";
    }
    return "TARGET";
}

const char* result_hint(services::LauncherResultKind kind) {
    switch (kind) {
    case services::LauncherResultKind::Application:
        return "Enter to launch";
    case services::LauncherResultKind::Command:
        return "Enter to execute · use > for explicit shell syntax";
    case services::LauncherResultKind::Action:
        return "Enter to run this Realmheart action";
    case services::LauncherResultKind::Emoji:
        return "Enter to copy";
    case services::LauncherResultKind::Clipboard:
        return "Enter to restore";
    }
    return "Enter to activate";
}

GtkWidget* make_panel_header(const char* eyebrow, const char* title) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    GtkWidget* eyebrow_label = gtk_label_new(eyebrow);
    gtk_label_set_xalign(GTK_LABEL(eyebrow_label), 0.0F);
    gtk_widget_add_css_class(eyebrow_label, "realmheart-launcher-eyebrow");

    GtkWidget* title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0F);
    gtk_widget_add_css_class(title_label, "realmheart-launcher-panel-title");

    gtk_box_append(GTK_BOX(box), eyebrow_label);
    gtk_box_append(GTK_BOX(box), title_label);
    return box;
}

GtkWidget* make_session_placeholder(
    const char* icon_name,
    const char* title,
    const char* subtitle
) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "realmheart-launcher-session-row");

    GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 34);
    gtk_widget_add_css_class(icon, "realmheart-launcher-session-icon");

    GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(labels, TRUE);

    GtkWidget* title_label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title_label, "realmheart-launcher-row-title");

    GtkWidget* subtitle_label = gtk_label_new(subtitle);
    gtk_label_set_xalign(GTK_LABEL(subtitle_label), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(subtitle_label, "realmheart-launcher-row-subtitle");

    gtk_box_append(GTK_BOX(labels), title_label);
    gtk_box_append(GTK_BOX(labels), subtitle_label);
    gtk_box_append(GTK_BOX(row), icon);
    gtk_box_append(GTK_BOX(row), labels);
    return row;
}

} // namespace

LauncherOverlay::LauncherOverlay(
    GtkApplication* app,
    services::LauncherService& service,
    services::WallpaperService& wallpaper_service
) : service_(service), wallpaper_service_(wallpaper_service) {
    window_ = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(window_), "realmheart-launcher-window");

    setup_window();
    setup_ui();
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

LauncherOverlay::~LauncherOverlay() {
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
    }
}

void LauncherOverlay::setup_window() {
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-launcher";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    spec.anchor_left = true;
    spec.anchor_right = true;
    apply_layer_surface(window_, spec);

    gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
}

void LauncherOverlay::setup_ui() {
    GtkWidget* root = gtk_overlay_new();
    gtk_widget_set_hexpand(root, TRUE);
    gtk_widget_set_vexpand(root, TRUE);
    gtk_widget_add_css_class(root, "realmheart-launcher-root");

    GtkWidget* dismiss = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(dismiss), FALSE);
    gtk_widget_set_hexpand(dismiss, TRUE);
    gtk_widget_set_vexpand(dismiss, TRUE);
    gtk_widget_add_css_class(dismiss, "realmheart-launcher-dismiss");
    g_signal_connect(dismiss, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<LauncherOverlay*>(data)->hide();
    }), this);
    gtk_overlay_set_child(GTK_OVERLAY(root), dismiss);

    // Left: deliberately quiet silhouette placeholder. Real Hyprland window
    // data lands in the next pass after the composition has earned its shape.
    GtkWidget* session_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_size_request(session_panel, 324, 480);
    gtk_widget_set_halign(session_panel, GTK_ALIGN_START);
    gtk_widget_set_valign(session_panel, GTK_ALIGN_START);
    gtk_widget_set_hexpand(session_panel, FALSE);
    gtk_widget_set_vexpand(session_panel, FALSE);
    gtk_widget_set_margin_start(session_panel, 68);
    gtk_widget_set_margin_top(session_panel, 188);
    gtk_widget_add_css_class(session_panel, "realmheart-launcher-panel");
    gtk_widget_add_css_class(session_panel, "realmheart-launcher-session-panel");
    gtk_box_append(GTK_BOX(session_panel), make_panel_header("CURRENT CONTEXT", "Session"));

    GtkWidget* session_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_vexpand(session_rows, TRUE);
    gtk_box_append(
        GTK_BOX(session_rows),
        make_session_placeholder("applications-system-symbolic", "Active applications", "Hyprland feed arrives next")
    );
    gtk_box_append(
        GTK_BOX(session_rows),
        make_session_placeholder("view-grid-symbolic", "Window context", "Reserved for compact titles")
    );
    gtk_box_append(GTK_BOX(session_panel), session_rows);

    // Centre: wallpaper aperture, embedded search, recommendations, and the
    // narrower search-results extension beneath it.
    GtkWidget* centre_column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(centre_column, 648, -1);
    gtk_widget_set_halign(centre_column, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(centre_column, GTK_ALIGN_START);
    gtk_widget_set_hexpand(centre_column, FALSE);
    gtk_widget_set_vexpand(centre_column, FALSE);
    gtk_widget_set_margin_top(centre_column, 86);
    gtk_widget_add_css_class(centre_column, "realmheart-launcher-centre-column");

    GtkWidget* centre_shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(centre_shell, 648, -1);
    gtk_widget_set_halign(centre_shell, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(centre_shell, FALSE);
    gtk_widget_add_css_class(centre_shell, "realmheart-launcher-centre-shell");

    GtkWidget* wallpaper_frame = gtk_overlay_new();
    gtk_widget_set_size_request(wallpaper_frame, 610, 162);
    gtk_widget_set_halign(wallpaper_frame, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(wallpaper_frame, FALSE);
    gtk_widget_set_margin_start(wallpaper_frame, 19);
    gtk_widget_set_margin_end(wallpaper_frame, 19);
    gtk_widget_set_margin_top(wallpaper_frame, 19);
    gtk_widget_add_css_class(wallpaper_frame, "realmheart-launcher-wallpaper-frame");
    gtk_widget_set_overflow(wallpaper_frame, GTK_OVERFLOW_HIDDEN);

    wallpaper_picture_ = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(wallpaper_picture_), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(wallpaper_picture_), TRUE);
    gtk_widget_set_hexpand(wallpaper_picture_, TRUE);
    gtk_widget_set_vexpand(wallpaper_picture_, TRUE);
    gtk_widget_add_css_class(wallpaper_picture_, "realmheart-launcher-wallpaper");

    // GtkPicture reports the wallpaper's full intrinsic size. If it is placed
    // directly in GtkOverlay, that natural size can inflate the entire centre
    // module to almost the monitor dimensions. A non-propagating viewport
    // makes the aperture's requested geometry authoritative.
    GtkWidget* wallpaper_viewport = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(wallpaper_viewport),
        GTK_POLICY_NEVER,
        GTK_POLICY_NEVER
    );
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(wallpaper_viewport),
        FALSE
    );
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(wallpaper_viewport),
        FALSE
    );
    gtk_widget_set_hexpand(wallpaper_viewport, TRUE);
    gtk_widget_set_vexpand(wallpaper_viewport, TRUE);
    gtk_widget_add_css_class(wallpaper_viewport, "realmheart-launcher-wallpaper-viewport");
    gtk_scrolled_window_set_child(
        GTK_SCROLLED_WINDOW(wallpaper_viewport),
        wallpaper_picture_
    );
    gtk_overlay_set_child(GTK_OVERLAY(wallpaper_frame), wallpaper_viewport);

    GtkWidget* wallpaper_shade = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(wallpaper_shade, GTK_ALIGN_FILL);
    gtk_widget_set_valign(wallpaper_shade, GTK_ALIGN_END);
    gtk_widget_set_size_request(wallpaper_shade, -1, 110);
    gtk_widget_set_can_target(wallpaper_shade, FALSE);
    gtk_widget_add_css_class(wallpaper_shade, "realmheart-launcher-wallpaper-shade");
    gtk_overlay_add_overlay(GTK_OVERLAY(wallpaper_frame), wallpaper_shade);

    search_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search_entry_),
        "Search applications or enter a command"
    );
    gtk_widget_set_size_request(search_entry_, 360, 50);
    gtk_widget_set_halign(search_entry_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(search_entry_, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(search_entry_, "realmheart-launcher-search");
    g_signal_connect(search_entry_, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
        static_cast<LauncherOverlay*>(data)->on_search_changed();
    }), this);
    g_signal_connect(search_entry_, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
        auto* overlay = static_cast<LauncherOverlay*>(data);
        if (!overlay->current_results_.empty()) {
            GtkListBoxRow* selected = gtk_list_box_get_selected_row(
                GTK_LIST_BOX(overlay->results_list_)
            );
            const int index = selected != nullptr
                ? gtk_list_box_row_get_index(selected)
                : 0;
            if (index >= 0) overlay->activate_result(static_cast<std::size_t>(index));
        } else if (!overlay->recommendations_.empty()) {
            overlay->activate_recommendation(0);
        }
    }), this);
    gtk_overlay_add_overlay(GTK_OVERLAY(wallpaper_frame), search_entry_);

    recommendations_revealer_ = gtk_revealer_new();
    gtk_revealer_set_transition_type(
        GTK_REVEALER(recommendations_revealer_),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP
    );
    gtk_revealer_set_transition_duration(GTK_REVEALER(recommendations_revealer_), 140);
    gtk_revealer_set_reveal_child(GTK_REVEALER(recommendations_revealer_), TRUE);

    GtkWidget* recommendation_region = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(recommendation_region, 19);
    gtk_widget_set_margin_end(recommendation_region, 19);
    gtk_widget_set_margin_top(recommendation_region, 8);
    gtk_widget_set_margin_bottom(recommendation_region, 10);

    GtkWidget* recommendation_label = gtk_label_new("QUICK LAUNCH");
    gtk_label_set_xalign(GTK_LABEL(recommendation_label), 0.0F);
    gtk_widget_add_css_class(recommendation_label, "realmheart-launcher-eyebrow");

    recommendations_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(recommendations_box_), TRUE);
    gtk_widget_add_css_class(recommendations_box_, "realmheart-launcher-recommendations");

    gtk_box_append(GTK_BOX(recommendation_region), recommendation_label);
    gtk_box_append(GTK_BOX(recommendation_region), recommendations_box_);
    gtk_revealer_set_child(GTK_REVEALER(recommendations_revealer_), recommendation_region);

    gtk_box_append(GTK_BOX(centre_shell), wallpaper_frame);
    gtk_box_append(GTK_BOX(centre_shell), recommendations_revealer_);
    gtk_box_append(GTK_BOX(centre_column), centre_shell);

    results_revealer_ = gtk_revealer_new();
    gtk_revealer_set_transition_type(
        GTK_REVEALER(results_revealer_),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN
    );
    gtk_revealer_set_transition_duration(GTK_REVEALER(results_revealer_), 170);

    GtkWidget* results_shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(results_shell, 520, -1);
    gtk_widget_set_halign(results_shell, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(results_shell, "realmheart-launcher-results-shell");

    results_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(results_list_), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(results_list_), FALSE);
    gtk_widget_add_css_class(results_list_, "realmheart-launcher-results-list");
    g_signal_connect(results_list_, "row-selected", G_CALLBACK(+[](
        GtkListBox*, GtkListBoxRow* row, gpointer data
    ) {
        static_cast<LauncherOverlay*>(data)->on_result_selected(row);
    }), this);
    g_signal_connect(results_list_, "row-activated", G_CALLBACK(+[](
        GtkListBox*, GtkListBoxRow* row, gpointer data
    ) {
        const int index = gtk_list_box_row_get_index(row);
        if (index >= 0) {
            static_cast<LauncherOverlay*>(data)->activate_result(
                static_cast<std::size_t>(index)
            );
        }
    }), this);

    GtkWidget* results_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(results_scroller),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC
    );
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(results_scroller),
        TRUE
    );
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(results_scroller),
        336
    );
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(results_scroller), results_list_);
    gtk_box_append(GTK_BOX(results_shell), results_scroller);
    gtk_revealer_set_child(GTK_REVEALER(results_revealer_), results_shell);
    gtk_box_append(GTK_BOX(centre_column), results_revealer_);

    // Right: a functional selection inspector from day one. Command capture
    // and full logs deliberately wait until the layout has been judged live.
    GtkWidget* inspector_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_size_request(inspector_panel, 360, 516);
    gtk_widget_set_halign(inspector_panel, GTK_ALIGN_END);
    gtk_widget_set_valign(inspector_panel, GTK_ALIGN_START);
    gtk_widget_set_hexpand(inspector_panel, FALSE);
    gtk_widget_set_vexpand(inspector_panel, FALSE);
    gtk_widget_set_margin_end(inspector_panel, 50);
    gtk_widget_set_margin_top(inspector_panel, 176);
    gtk_widget_add_css_class(inspector_panel, "realmheart-launcher-panel");
    gtk_widget_add_css_class(inspector_panel, "realmheart-launcher-inspector-panel");
    gtk_box_append(GTK_BOX(inspector_panel), make_panel_header("SELECTED TARGET", "Inspector"));

    GtkWidget* inspector_identity = gtk_box_new(GTK_ORIENTATION_VERTICAL, 11);
    gtk_widget_set_vexpand(inspector_identity, TRUE);
    gtk_widget_add_css_class(inspector_identity, "realmheart-launcher-inspector-identity");

    inspector_icon_ = gtk_image_new_from_icon_name("application-x-executable-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(inspector_icon_), 70);
    gtk_widget_set_halign(inspector_icon_, GTK_ALIGN_START);
    gtk_widget_add_css_class(inspector_icon_, "realmheart-launcher-inspector-icon");

    inspector_kind_ = gtk_label_new("APPLICATION");
    gtk_label_set_xalign(GTK_LABEL(inspector_kind_), 0.0F);
    gtk_widget_add_css_class(inspector_kind_, "realmheart-launcher-inspector-kind");

    inspector_title_ = gtk_label_new("Select an application");
    gtk_label_set_xalign(GTK_LABEL(inspector_title_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(inspector_title_), TRUE);
    gtk_widget_add_css_class(inspector_title_, "realmheart-launcher-inspector-title");

    inspector_subtitle_ = gtk_label_new("Search results and recommendations will describe themselves here.");
    gtk_label_set_xalign(GTK_LABEL(inspector_subtitle_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(inspector_subtitle_), TRUE);
    gtk_widget_add_css_class(inspector_subtitle_, "realmheart-launcher-inspector-subtitle");

    gtk_box_append(GTK_BOX(inspector_identity), inspector_icon_);
    gtk_box_append(GTK_BOX(inspector_identity), inspector_kind_);
    gtk_box_append(GTK_BOX(inspector_identity), inspector_title_);
    gtk_box_append(GTK_BOX(inspector_identity), inspector_subtitle_);
    gtk_box_append(GTK_BOX(inspector_panel), inspector_identity);

    inspector_hint_ = gtk_label_new("Type to inspect a result");
    gtk_label_set_xalign(GTK_LABEL(inspector_hint_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(inspector_hint_), TRUE);
    gtk_widget_add_css_class(inspector_hint_, "realmheart-launcher-inspector-hint");
    gtk_box_append(GTK_BOX(inspector_panel), inspector_hint_);

    // Independent overlay siblings: each panel owns its position and size.
    // The centre is added first so the detached side panels remain above it
    // even if a future geometry regression causes accidental overlap.
    gtk_overlay_add_overlay(GTK_OVERLAY(root), centre_column);
    gtk_overlay_add_overlay(GTK_OVERLAY(root), session_panel);
    gtk_overlay_add_overlay(GTK_OVERLAY(root), inspector_panel);

    gtk_window_set_child(window_, root);

    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(+[](
        GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data
    ) -> gboolean {
        return static_cast<LauncherOverlay*>(data)->handle_key(keyval);
    }), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), key_controller);
}

void LauncherOverlay::refresh_wallpaper() {
    const auto path = wallpaper_service_.load_path();
    if (!path) {
        gtk_picture_set_paintable(GTK_PICTURE(wallpaper_picture_), nullptr);
        gtk_widget_add_css_class(wallpaper_picture_, "realmheart-launcher-wallpaper-missing");
        return;
    }

    gtk_widget_remove_css_class(wallpaper_picture_, "realmheart-launcher-wallpaper-missing");
    GFile* file = g_file_new_for_path(path->c_str());
    gtk_picture_set_file(GTK_PICTURE(wallpaper_picture_), file);
    g_object_unref(file);
}

void LauncherOverlay::refresh_idle_content() {
    recommendations_ = service_.recommendations(kRecommendationCount);
    rebuild_recommendations();
    update_inspector(recommendations_.empty() ? nullptr : &recommendations_.front());
}

void LauncherOverlay::rebuild_recommendations() {
    clear_box(recommendations_box_);

    for (std::size_t index = 0; index < recommendations_.size(); ++index) {
        const auto& result = recommendations_[index];
        GtkWidget* button = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
        gtk_widget_add_css_class(button, "realmheart-launcher-recommendation");
        g_object_set_data(G_OBJECT(button), "realmheart-recommendation-index", GSIZE_TO_POINTER(index + 1));

        GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget* icon = gtk_image_new_from_icon_name(
            result.icon_name.empty() ? "application-x-executable" : result.icon_name.c_str()
        );
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 28);

        GtkWidget* label = gtk_label_new(result.title.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 11);
        gtk_widget_add_css_class(label, "realmheart-launcher-recommendation-label");

        gtk_box_append(GTK_BOX(content), icon);
        gtk_box_append(GTK_BOX(content), label);
        gtk_button_set_child(GTK_BUTTON(button), content);
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* source, gpointer data) {
            const auto stored = GPOINTER_TO_SIZE(
                g_object_get_data(G_OBJECT(source), "realmheart-recommendation-index")
            );
            if (stored > 0) {
                static_cast<LauncherOverlay*>(data)->activate_recommendation(stored - 1);
            }
        }), this);
        gtk_box_append(GTK_BOX(recommendations_box_), button);
    }

    if (recommendations_.empty()) {
        GtkWidget* empty = gtk_label_new("No launchable applications indexed");
        gtk_widget_add_css_class(empty, "realmheart-launcher-empty");
        gtk_box_append(GTK_BOX(recommendations_box_), empty);
    }
}

void LauncherOverlay::rebuild_results() {
    clear_list(results_list_);

    for (const auto& result : current_results_) {
        GtkWidget* row = gtk_list_box_row_new();
        gtk_widget_add_css_class(row, "realmheart-launcher-result-row");

        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
        gtk_widget_set_margin_start(box, 16);
        gtk_widget_set_margin_end(box, 16);
        gtk_widget_set_margin_top(box, 10);
        gtk_widget_set_margin_bottom(box, 10);

        GtkWidget* icon = gtk_image_new_from_icon_name(
            result.icon_name.empty() ? "application-x-executable" : result.icon_name.c_str()
        );
        gtk_image_set_pixel_size(GTK_IMAGE(icon), 36);

        GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_hexpand(labels, TRUE);

        GtkWidget* title = gtk_label_new(result.title.c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(title, "realmheart-launcher-row-title");

        GtkWidget* subtitle = gtk_label_new(result.subtitle.c_str());
        gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(subtitle, "realmheart-launcher-row-subtitle");
        gtk_widget_set_visible(subtitle, !result.subtitle.empty());

        GtkWidget* kind = gtk_label_new(result_kind_label(result.kind));
        gtk_widget_set_valign(kind, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(kind, "realmheart-launcher-result-kind");

        gtk_box_append(GTK_BOX(labels), title);
        gtk_box_append(GTK_BOX(labels), subtitle);
        gtk_box_append(GTK_BOX(box), icon);
        gtk_box_append(GTK_BOX(box), labels);
        gtk_box_append(GTK_BOX(box), kind);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(results_list_), row);
    }

    if (current_results_.empty()) {
        GtkWidget* row = gtk_list_box_row_new();
        gtk_widget_set_sensitive(row, FALSE);
        GtkWidget* empty = gtk_label_new("No application or valid command found");
        gtk_widget_set_margin_top(empty, 28);
        gtk_widget_set_margin_bottom(empty, 28);
        gtk_widget_add_css_class(empty, "realmheart-launcher-empty");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), empty);
        gtk_list_box_append(GTK_LIST_BOX(results_list_), row);
        update_inspector(nullptr);
        return;
    }

    GtkListBoxRow* first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(results_list_), 0);
    gtk_list_box_select_row(GTK_LIST_BOX(results_list_), first);
}

void LauncherOverlay::on_search_changed() {
    const char* raw = gtk_editable_get_text(GTK_EDITABLE(search_entry_));
    const std::string query = raw != nullptr ? raw : "";
    const bool searching = query.find_first_not_of(" \t\n\r") != std::string::npos;

    gtk_revealer_set_reveal_child(
        GTK_REVEALER(recommendations_revealer_),
        !searching
    );
    gtk_revealer_set_reveal_child(GTK_REVEALER(results_revealer_), searching);

    if (!searching) {
        current_results_.clear();
        clear_list(results_list_);
        update_inspector(recommendations_.empty() ? nullptr : &recommendations_.front());
        return;
    }

    current_results_ = service_.search(query, kResultCount);
    rebuild_results();
}

void LauncherOverlay::on_result_selected(GtkListBoxRow* row) {
    if (row == nullptr) {
        update_inspector(nullptr);
        return;
    }
    const int index = gtk_list_box_row_get_index(row);
    if (index < 0 || static_cast<std::size_t>(index) >= current_results_.size()) {
        update_inspector(nullptr);
        return;
    }
    update_inspector(&current_results_[static_cast<std::size_t>(index)]);
}

void LauncherOverlay::update_inspector(const services::LauncherResult* result) {
    if (result == nullptr) {
        gtk_image_set_from_icon_name(
            GTK_IMAGE(inspector_icon_),
            "system-search-symbolic"
        );
        gtk_label_set_text(GTK_LABEL(inspector_kind_), "NO TARGET");
        gtk_label_set_text(GTK_LABEL(inspector_title_), "Nothing selected");
        gtk_label_set_text(
            GTK_LABEL(inspector_subtitle_),
            "Try another application name or use > for an explicit command."
        );
        gtk_label_set_text(GTK_LABEL(inspector_hint_), "Search remains focused");
        return;
    }

    gtk_image_set_from_icon_name(
        GTK_IMAGE(inspector_icon_),
        result->icon_name.empty() ? "application-x-executable" : result->icon_name.c_str()
    );
    gtk_label_set_text(GTK_LABEL(inspector_kind_), result_kind_label(result->kind));
    gtk_label_set_text(GTK_LABEL(inspector_title_), result->title.c_str());
    gtk_label_set_text(
        GTK_LABEL(inspector_subtitle_),
        result->subtitle.empty() ? "Ready to activate" : result->subtitle.c_str()
    );
    gtk_label_set_text(GTK_LABEL(inspector_hint_), result_hint(result->kind));
}

void LauncherOverlay::activate_result(std::size_t index) {
    if (index >= current_results_.size()) return;
    if (service_.activate(current_results_[index])) hide();
}

void LauncherOverlay::activate_recommendation(std::size_t index) {
    if (index >= recommendations_.size()) return;
    if (service_.activate(recommendations_[index])) hide();
}

bool LauncherOverlay::handle_key(guint keyval) {
    if (keyval == GDK_KEY_Escape) {
        const char* text = gtk_editable_get_text(GTK_EDITABLE(search_entry_));
        if (text != nullptr && *text != '\0') {
            gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
            return true;
        }
        hide();
        return true;
    }

    if ((keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) && !current_results_.empty()) {
        GtkListBoxRow* selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(results_list_));
        int index = selected != nullptr ? gtk_list_box_row_get_index(selected) : 0;
        index += keyval == GDK_KEY_Down ? 1 : -1;
        index = std::clamp(index, 0, static_cast<int>(current_results_.size()) - 1);
        gtk_list_box_select_row(
            GTK_LIST_BOX(results_list_),
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(results_list_), index)
        );
        return true;
    }

    return false;
}

void LauncherOverlay::toggle() {
    if (gtk_widget_get_visible(GTK_WIDGET(window_))) {
        hide();
    } else {
        show();
    }
}

void LauncherOverlay::show() {
    refresh_wallpaper();
    refresh_idle_content();
    gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
    on_search_changed();
    gtk_window_present(window_);
    gtk_widget_grab_focus(search_entry_);
}

void LauncherOverlay::hide() {
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

} // namespace realmheart::ui
