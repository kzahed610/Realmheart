#include "ui/powermenu/PowerMenuScene.hpp"

#include "ui/AssetResolver.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

namespace realmheart::ui::powermenu {
namespace {

using SnapshotDrawFunc = void (*)(GtkWidget*, GtkSnapshot*, gpointer);

typedef struct _RealmheartPowerMenuSceneWidget {
    GtkWidget parent_instance;
    SnapshotDrawFunc draw_func;
    gpointer user_data;
} RealmheartPowerMenuSceneWidget;

typedef struct _RealmheartPowerMenuSceneWidgetClass {
    GtkWidgetClass parent_class;
} RealmheartPowerMenuSceneWidgetClass;

G_DEFINE_TYPE(
    RealmheartPowerMenuSceneWidget,
    realmheart_power_menu_scene_widget,
    GTK_TYPE_WIDGET
)

void realmheart_power_menu_scene_widget_snapshot(
    GtkWidget* widget,
    GtkSnapshot* snapshot
) {
    auto* self = reinterpret_cast<RealmheartPowerMenuSceneWidget*>(widget);
    if (self->draw_func != nullptr) {
        self->draw_func(widget, snapshot, self->user_data);
    }
}

void realmheart_power_menu_scene_widget_class_init(
    RealmheartPowerMenuSceneWidgetClass* klass
) {
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = realmheart_power_menu_scene_widget_snapshot;
    gtk_widget_class_set_css_name(widget_class, "realmheart-power-menu-scene");
}

void realmheart_power_menu_scene_widget_init(
    RealmheartPowerMenuSceneWidget* self
) {
    self->draw_func = nullptr;
    self->user_data = nullptr;
}

GtkWidget* scene_widget_new(
    SnapshotDrawFunc draw_func,
    gpointer user_data
) {
    auto* self = reinterpret_cast<RealmheartPowerMenuSceneWidget*>(
        g_object_new(realmheart_power_menu_scene_widget_get_type(), nullptr)
    );
    self->draw_func = draw_func;
    self->user_data = user_data;
    return GTK_WIDGET(self);
}

void scene_widget_clear(GtkWidget* widget) {
    if (widget == nullptr) return;
    auto* self = reinterpret_cast<RealmheartPowerMenuSceneWidget*>(widget);
    self->draw_func = nullptr;
    self->user_data = nullptr;
}

constexpr std::array<std::string_view, 13> kNeutralLayerOrder{
    "static-base",
    "arthur-hair-loose-01",
    "arthur-hair-loose-02",
    "arthur-hair-back",
    "sylvie-hair-loose-02",
    "sylvie-hair-loose-01",
    "sylvie-hair-back",
    "sylvie-hair-side",
    "smoke-aether",
    "smoke-mana",
    "dust-far",
    "dust-mid",
    "dust-front",
};

void append_background(GtkSnapshot* snapshot, int width, int height) {
    const GdkRGBA background{0.005, 0.004, 0.012, 1.0};
    graphene_rect_t bounds;
    graphene_rect_init(
        &bounds,
        0.0F,
        0.0F,
        static_cast<float>(std::max(width, 0)),
        static_cast<float>(std::max(height, 0))
    );
    gtk_snapshot_append_color(snapshot, &background, &bounds);
}

} // namespace

PowerMenuScene::PowerMenuScene() {
    widget_ = scene_widget_new(&PowerMenuScene::snapshot_callback, this);
    g_object_ref_sink(widget_);
    gtk_widget_set_hexpand(widget_, TRUE);
    gtk_widget_set_vexpand(widget_, TRUE);
    gtk_widget_set_can_target(widget_, FALSE);

    if (!load()) {
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
    }
}

PowerMenuScene::~PowerMenuScene() {
    scene_widget_clear(widget_);
    release_layers();
    manifest_.reset();
    if (widget_ != nullptr) {
        g_object_unref(widget_);
        widget_ = nullptr;
    }
}

GtkWidget* PowerMenuScene::widget() const {
    return widget_;
}

bool PowerMenuScene::ready() const {
    return manifest_.has_value() && layers_.size() == kNeutralLayerOrder.size();
}

const std::string& PowerMenuScene::error_message() const {
    return error_message_;
}

void PowerMenuScene::snapshot_callback(
    GtkWidget* widget,
    GtkSnapshot* snapshot,
    gpointer user_data
) {
    auto* self = static_cast<PowerMenuScene*>(user_data);
    if (self != nullptr) self->snapshot(widget, snapshot);
}

bool PowerMenuScene::load() {
    error_message_.clear();
    release_layers();
    manifest_.reset();

    const auto manifest_path = resolve_project_asset("power-menu/manifest.json");
    if (!manifest_path) {
        error_message_ = "Unable to resolve power-menu/manifest.json";
        return false;
    }

    auto loaded_manifest = PowerMenuManifest::load(
        *manifest_path,
        &error_message_
    );
    if (!loaded_manifest) return false;

    std::vector<LoadedLayer> loaded_layers;
    loaded_layers.reserve(kNeutralLayerOrder.size());

    for (const std::string_view layer_name : kNeutralLayerOrder) {
        const PowerMenuAsset* asset = loaded_manifest->find_asset(layer_name);
        if (asset == nullptr) {
            error_message_ = "Neutral scene references missing asset: " +
                std::string(layer_name);
            for (auto& layer : loaded_layers) {
                if (layer.texture != nullptr) g_object_unref(layer.texture);
                layer.texture = nullptr;
            }
            return false;
        }

        GError* texture_error = nullptr;
        GdkTexture* texture = gdk_texture_new_from_filename(
            asset->path_1x.c_str(),
            &texture_error
        );
        if (texture == nullptr) {
            error_message_ = "Unable to load power-menu texture '" + asset->name + "'";
            if (texture_error != nullptr) {
                error_message_ += ": ";
                error_message_ += texture_error->message;
                g_error_free(texture_error);
            }
            for (auto& layer : loaded_layers) {
                if (layer.texture != nullptr) g_object_unref(layer.texture);
                layer.texture = nullptr;
            }
            return false;
        }

        if (gdk_texture_get_width(texture) != asset->placement_1x.width ||
            gdk_texture_get_height(texture) != asset->placement_1x.height) {
            error_message_ = "Power-menu texture dimensions disagree with manifest: " +
                asset->name;
            g_object_unref(texture);
            for (auto& layer : loaded_layers) {
                if (layer.texture != nullptr) g_object_unref(layer.texture);
                layer.texture = nullptr;
            }
            return false;
        }

        loaded_layers.push_back({
            .name = asset->name,
            .texture = texture,
            .placement = asset->placement_1x,
        });
    }

    manifest_ = std::move(*loaded_manifest);
    layers_ = std::move(loaded_layers);
    gtk_widget_queue_draw(widget_);

    std::cerr << "[PowerMenuScene] Loaded " << layers_.size()
              << " neutral 1x layers from " << manifest_path->string() << '\n';
    return true;
}

void PowerMenuScene::snapshot(GtkWidget* widget, GtkSnapshot* snapshot) const {
    const int width = gtk_widget_get_width(widget);
    const int height = gtk_widget_get_height(widget);
    append_background(snapshot, width, height);

    if (!ready() || width <= 0 || height <= 0 ||
        manifest_->logical_canvas.width <= 0 ||
        manifest_->logical_canvas.height <= 0) {
        return;
    }

    const double scale = std::max(
        static_cast<double>(width) /
            static_cast<double>(manifest_->logical_canvas.width),
        static_cast<double>(height) /
            static_cast<double>(manifest_->logical_canvas.height)
    );
    const double offset_x = (
        static_cast<double>(width) -
        static_cast<double>(manifest_->logical_canvas.width) * scale
    ) * 0.5;
    const double offset_y = (
        static_cast<double>(height) -
        static_cast<double>(manifest_->logical_canvas.height) * scale
    ) * 0.5;

    graphene_point_t translation;
    graphene_point_init(
        &translation,
        static_cast<float>(offset_x),
        static_cast<float>(offset_y)
    );

    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &translation);
    gtk_snapshot_scale(
        snapshot,
        static_cast<float>(scale),
        static_cast<float>(scale)
    );

    for (const auto& layer : layers_) {
        graphene_rect_t bounds;
        graphene_rect_init(
            &bounds,
            static_cast<float>(layer.placement.x),
            static_cast<float>(layer.placement.y),
            static_cast<float>(layer.placement.width),
            static_cast<float>(layer.placement.height)
        );
        gtk_snapshot_append_scaled_texture(
            snapshot,
            layer.texture,
            GSK_SCALING_FILTER_LINEAR,
            &bounds
        );
    }

    gtk_snapshot_restore(snapshot);
}

void PowerMenuScene::release_layers() noexcept {
    for (auto& layer : layers_) {
        if (layer.texture != nullptr) g_object_unref(layer.texture);
        layer.texture = nullptr;
    }
    layers_.clear();
}

} // namespace realmheart::ui::powermenu
