#include "ui/powermenu/PowerMenuScene.hpp"

#include "animation/layered/FlowWarp.hpp"
#include "ui/AssetResolver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

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

constexpr double kIdleFrameSeconds = 1.0 / 15.0;

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
constexpr GdkMemoryFormat kCairoArgb32MemoryFormat =
    GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
constexpr GdkMemoryFormat kCairoArgb32MemoryFormat =
    GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif

struct CairoSurfaceDeleter {
    void operator()(cairo_surface_t* surface) const noexcept {
        if (surface != nullptr) cairo_surface_destroy(surface);
    }
};
using SurfacePtr = std::unique_ptr<cairo_surface_t, CairoSurfaceDeleter>;

SurfacePtr load_argb32_surface(
    const std::filesystem::path& path,
    int expected_width,
    int expected_height,
    std::string_view label,
    std::string* error_message
) {
    SurfacePtr surface(cairo_image_surface_create_from_png(path.c_str()));
    if (!surface || cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS ||
        cairo_image_surface_get_format(surface.get()) != CAIRO_FORMAT_ARGB32 ||
        cairo_image_surface_get_width(surface.get()) != expected_width ||
        cairo_image_surface_get_height(surface.get()) != expected_height) {
        if (error_message != nullptr) {
            *error_message = "Unable to decode ARGB32 " + std::string(label) +
                ": " + path.string();
        }
        return {};
    }
    cairo_surface_flush(surface.get());
    return surface;
}

GdkTexture* texture_from_argb32(
    const std::uint8_t* pixels,
    int width,
    int height,
    int stride
) {
    if (pixels == nullptr || width <= 0 || height <= 0 || stride < width * 4) {
        return nullptr;
    }
    const gsize byte_count = static_cast<gsize>(stride) * static_cast<gsize>(height);
    GBytes* bytes = g_bytes_new(pixels, byte_count);
    GdkTexture* texture = gdk_memory_texture_new(
        width, height, kCairoArgb32MemoryFormat, bytes, static_cast<gsize>(stride)
    );
    g_bytes_unref(bytes);
    return texture;
}

GdkTexture* tint_mask_texture(
    cairo_surface_t* surface,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue
) {
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int source_stride = cairo_image_surface_get_stride(surface);
    const int target_stride = width * 4;
    const auto* source = cairo_image_surface_get_data(surface);
    std::vector<std::uint8_t> tinted(
        static_cast<std::size_t>(target_stride * height), 0U
    );
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint32_t source_word = 0;
            std::memcpy(&source_word, source + (y * source_stride) + (x * 4), 4U);
            const std::uint32_t alpha = (source_word >> 24U) & 0xffU;
            const std::uint32_t target_word = (alpha << 24U) |
                (((static_cast<std::uint32_t>(red) * alpha + 127U) / 255U) << 16U) |
                (((static_cast<std::uint32_t>(green) * alpha + 127U) / 255U) << 8U) |
                ((static_cast<std::uint32_t>(blue) * alpha + 127U) / 255U);
            std::memcpy(tinted.data() + (y * target_stride) + (x * 4), &target_word, 4U);
        }
    }
    return texture_from_argb32(tinted.data(), width, height, target_stride);
}

void append_texture(
    GtkSnapshot* snapshot,
    GdkTexture* texture,
    const PowerMenuPlacement& placement,
    const PowerMenuLayerMotionSample& motion,
    PowerMenuVector pivot
) {
    if (texture == nullptr || motion.opacity <= 0.0001) return;
    gtk_snapshot_save(snapshot);
    graphene_point_t translation = GRAPHENE_POINT_INIT(
        static_cast<float>(placement.x + motion.translation_x),
        static_cast<float>(placement.y + motion.translation_y)
    );
    gtk_snapshot_translate(snapshot, &translation);
    const double pivot_x = static_cast<double>(placement.width) * pivot.x;
    const double pivot_y = static_cast<double>(placement.height) * pivot.y;
    if (std::abs(motion.rotation_degrees) > 0.0001 ||
        std::abs(motion.scale - 1.0) > 0.0001) {
        translation = GRAPHENE_POINT_INIT(
            static_cast<float>(pivot_x), static_cast<float>(pivot_y)
        );
        gtk_snapshot_translate(snapshot, &translation);
        if (std::abs(motion.rotation_degrees) > 0.0001) {
            gtk_snapshot_rotate(snapshot, static_cast<float>(motion.rotation_degrees));
        }
        if (std::abs(motion.scale - 1.0) > 0.0001) {
            gtk_snapshot_scale(
                snapshot, static_cast<float>(motion.scale), static_cast<float>(motion.scale)
            );
        }
        translation = GRAPHENE_POINT_INIT(
            static_cast<float>(-pivot_x), static_cast<float>(-pivot_y)
        );
        gtk_snapshot_translate(snapshot, &translation);
    }
    if (motion.opacity < 0.9999) {
        gtk_snapshot_push_opacity(snapshot, static_cast<float>(motion.opacity));
    }
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(
        0.0F, 0.0F,
        static_cast<float>(placement.width), static_cast<float>(placement.height)
    );
    gtk_snapshot_append_scaled_texture(
        snapshot, texture, GSK_SCALING_FILTER_LINEAR, &bounds
    );
    if (motion.opacity < 0.9999) gtk_snapshot_pop(snapshot);
    gtk_snapshot_restore(snapshot);
}

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
}

PowerMenuScene::~PowerMenuScene() {
    stop_tick();
    on_hidden_ = {};
    visibility_callback_ = {};
    scene_widget_clear(widget_);
    release_layers();
    animator_.reset();
    rig_.reset();
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
    return manifest_.has_value() && rig_.has_value() && animator_ != nullptr &&
        layers_.size() == rig_->layers.size();
}

const std::string& PowerMenuScene::error_message() const {
    return error_message_;
}

void PowerMenuScene::set_visibility_callback(std::function<void(double)> callback) {
    visibility_callback_ = std::move(callback);
    publish_visibility();
}

void PowerMenuScene::present() {
    on_hidden_ = {};
    if (!ready() && !load()) {
        std::cerr << "[PowerMenuScene] " << error_message_ << '\n';
        return;
    }
    animator_->open();
    publish_visibility();
    gtk_widget_queue_draw(widget_);
    ensure_tick();
}

void PowerMenuScene::dismiss(std::function<void()> on_hidden) {
    on_hidden_ = std::move(on_hidden);
    if (!animator_ || animator_->phase() == PowerMenuScenePhase::Hidden) {
        auto callback = std::move(on_hidden_);
        if (callback) callback();
        return;
    }
    animator_->close();
    gtk_widget_queue_draw(widget_);
    ensure_tick();
}

void PowerMenuScene::hide_immediately() {
    stop_tick();
    on_hidden_ = {};
    if (rig_) animator_ = std::make_unique<PowerMenuAnimator>(*rig_);
    publish_visibility();
    gtk_widget_queue_draw(widget_);
}

void PowerMenuScene::set_confirming(bool confirming) {
    if (!animator_) return;
    animator_->set_confirming(confirming);
    gtk_widget_queue_draw(widget_);
}

void PowerMenuScene::snapshot_callback(
    GtkWidget* widget,
    GtkSnapshot* snapshot,
    gpointer user_data
) {
    auto* self = static_cast<PowerMenuScene*>(user_data);
    if (self != nullptr) self->snapshot(widget, snapshot);
}

gboolean PowerMenuScene::timer_callback(gpointer user_data) {
    auto* self = static_cast<PowerMenuScene*>(user_data);
    return self == nullptr ? G_SOURCE_REMOVE : self->on_timer();
}

bool PowerMenuScene::load() {
    const auto started = std::chrono::steady_clock::now();
    error_message_.clear();
    release_layers();
    animator_.reset();
    rig_.reset();
    manifest_.reset();

    const auto manifest_path = resolve_project_asset("power-menu/manifest.json");
    if (!manifest_path) {
        error_message_ = "Unable to resolve power-menu/manifest.json";
        return false;
    }
    auto loaded_manifest = PowerMenuManifest::load(*manifest_path, &error_message_);
    if (!loaded_manifest) return false;
    auto loaded_rig = PowerMenuRig::load(
        manifest_path->parent_path() / "rig.json", *loaded_manifest, &error_message_
    );
    if (!loaded_rig) return false;

    std::vector<LoadedLayer> loaded_layers;
    loaded_layers.reserve(loaded_rig->layers.size());
    const auto release_temporary = [&loaded_layers]() {
        for (auto& layer : loaded_layers) {
            if (layer.texture != nullptr) g_object_unref(layer.texture);
            for (GdkTexture* pose : layer.flow_poses) {
                if (pose != nullptr) g_object_unref(pose);
            }
        }
        loaded_layers.clear();
    };
    std::size_t retained_bytes = 0U;
    std::size_t texture_count = 0U;

    for (const PowerMenuRigLayer& rig_layer : loaded_rig->layers) {
        const PowerMenuAsset* asset = loaded_manifest->find_asset(rig_layer.asset);
        if (asset == nullptr) {
            error_message_ = "Animated scene references missing asset: " + rig_layer.asset;
            release_temporary();
            return false;
        }

        GError* texture_error = nullptr;
        GdkTexture* texture = gdk_texture_new_from_filename(
            asset->path_1x.c_str(), &texture_error
        );
        if (texture == nullptr) {
            error_message_ = "Unable to load power-menu texture '" + asset->name + "'";
            if (texture_error != nullptr) {
                error_message_ += ": ";
                error_message_ += texture_error->message;
                g_error_free(texture_error);
            }
            release_temporary();
            return false;
        }
        if (gdk_texture_get_width(texture) != asset->placement_1x.width ||
            gdk_texture_get_height(texture) != asset->placement_1x.height) {
            error_message_ = "Power-menu texture dimensions disagree with manifest: " +
                asset->name;
            g_object_unref(texture);
            release_temporary();
            return false;
        }

        LoadedLayer layer{
            .name = asset->name,
            .texture = texture,
            .flow_poses = {},
            .mesh = std::nullopt,
            .animation = rig_layer.animation,
            .placement = asset->placement_1x,
            .flow_minimum = 0.0,
            .flow_maximum = 0.0,
            .decoded_bytes = static_cast<std::size_t>(asset->placement_1x.width) *
                static_cast<std::size_t>(asset->placement_1x.height) * 4U,
        };
        ++texture_count;

        if (layer.animation.type == PowerMenuAnimationType::GlowMask) {
            auto source = load_argb32_surface(
                asset->path_1x, asset->placement_1x.width,
                asset->placement_1x.height, "glow mask", &error_message_
            );
            if (!source) {
                g_object_unref(layer.texture);
                release_temporary();
                return false;
            }
            const bool iris = layer.animation.tint_role == "iris-gold";
            GdkTexture* tinted = tint_mask_texture(
                source.get(), iris ? 246U : 248U, iris ? 198U : 223U,
                iris ? 91U : 156U
            );
            if (tinted == nullptr) {
                error_message_ = "Unable to tint power-menu glow mask: " + asset->name;
                g_object_unref(layer.texture);
                release_temporary();
                return false;
            }
            g_object_unref(layer.texture);
            layer.texture = tinted;
        }

        const bool uses_flow =
            layer.animation.type == PowerMenuAnimationType::MeshFlow ||
            layer.animation.type == PowerMenuAnimationType::FlowDrift;
        if (uses_flow) {
            auto source = load_argb32_surface(
                asset->path_1x, asset->placement_1x.width,
                asset->placement_1x.height, "flow source", &error_message_
            );
            auto mask = load_argb32_surface(
                layer.animation.movement_mask_path, asset->placement_1x.width,
                asset->placement_1x.height, "movement mask", &error_message_
            );
            auto flow = load_argb32_surface(
                layer.animation.flow_map_path, asset->placement_1x.width,
                asset->placement_1x.height, "flow map", &error_message_
            );
            if (!source || !mask || !flow) {
                g_object_unref(layer.texture);
                release_temporary();
                return false;
            }

            const int width = asset->placement_1x.width;
            const int height = asset->placement_1x.height;
            const animation::layered::Argb32ImageView source_view{
                cairo_image_surface_get_data(source.get()), width, height,
                cairo_image_surface_get_stride(source.get())
            };
            const animation::layered::Argb32ImageView flow_view{
                cairo_image_surface_get_data(flow.get()), width, height,
                cairo_image_surface_get_stride(flow.get())
            };
            const animation::layered::Argb32ImageView mask_view{
                cairo_image_surface_get_data(mask.get()), width, height,
                cairo_image_surface_get_stride(mask.get())
            };
            const int pose_count = layer.animation.flow.pose_count;
            const double amplitude = layer.animation.type == PowerMenuAnimationType::MeshFlow
                ? layer.animation.flow.amplitude : 2.0;
            layer.flow_minimum = -amplitude;
            layer.flow_maximum = amplitude;
            layer.flow_poses.reserve(static_cast<std::size_t>(pose_count));
            for (int pose_index = 0; pose_index < pose_count; ++pose_index) {
                const double fraction = static_cast<double>(pose_index) /
                    static_cast<double>(pose_count - 1);
                const double displacement = layer.flow_minimum +
                    ((layer.flow_maximum - layer.flow_minimum) * fraction);
                auto pixels = animation::layered::warp_argb32(
                    source_view, flow_view, mask_view, displacement, &error_message_
                );
                if (!pixels) {
                    g_object_unref(layer.texture);
                    for (GdkTexture* pose : layer.flow_poses) g_object_unref(pose);
                    release_temporary();
                    return false;
                }
                GdkTexture* pose = texture_from_argb32(
                    pixels->data(), width, height, width * 4
                );
                if (pose == nullptr) {
                    error_message_ = "Unable to create power-menu flow pose: " + asset->name;
                    g_object_unref(layer.texture);
                    for (GdkTexture* loaded_pose : layer.flow_poses) {
                        g_object_unref(loaded_pose);
                    }
                    release_temporary();
                    return false;
                }
                layer.flow_poses.push_back(pose);
                layer.decoded_bytes += static_cast<std::size_t>(width) *
                    static_cast<std::size_t>(height) * 4U;
                ++texture_count;
            }

            if (layer.animation.type == PowerMenuAnimationType::MeshFlow) {
                const auto axis = layer.animation.mesh.strip_axis == PowerMenuStripAxis::Rows
                    ? animation::layered::StripAxis::Rows
                    : animation::layered::StripAxis::Columns;
                animation::layered::AnchorPolicy anchor =
                    animation::layered::AnchorPolicy::WeightedTranslate;
                if (layer.animation.mesh.anchor_mode == PowerMenuAnchorMode::PinnedMinimum) {
                    anchor = animation::layered::AnchorPolicy::PinnedMinimum;
                } else if (layer.animation.mesh.anchor_mode ==
                           PowerMenuAnchorMode::PinnedMaximum) {
                    anchor = animation::layered::AnchorPolicy::PinnedMaximum;
                }
                layer.mesh = animation::layered::DirectionalStripMesh::from_argb32(
                    cairo_image_surface_get_data(mask.get()), width, height,
                    cairo_image_surface_get_stride(mask.get()),
                    layer.animation.mesh.strip_count, axis, anchor,
                    {layer.animation.mesh.direction.x, layer.animation.mesh.direction.y},
                    &error_message_
                );
                if (!layer.mesh) {
                    g_object_unref(layer.texture);
                    for (GdkTexture* pose : layer.flow_poses) g_object_unref(pose);
                    release_temporary();
                    return false;
                }
            }

            g_object_unref(layer.texture);
            layer.texture = nullptr;
            --texture_count;
            layer.decoded_bytes -= static_cast<std::size_t>(width) *
                static_cast<std::size_t>(height) * 4U;
        }

        retained_bytes += layer.decoded_bytes;
        loaded_layers.push_back(std::move(layer));
    }

    manifest_ = std::move(*loaded_manifest);
    rig_ = std::move(*loaded_rig);
    layers_ = std::move(loaded_layers);
    animator_ = std::make_unique<PowerMenuAnimator>(*rig_);
    gtk_widget_queue_draw(widget_);

    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    ).count();
    std::cerr << "[PowerMenuScene] quality=authored-1x layers=" << layers_.size()
              << " textures=" << texture_count
              << " decodedMiB=" << (static_cast<double>(retained_bytes) / 1048576.0)
              << " prewarmMs=" << load_ms << '\n';
    for (const auto& layer : layers_) {
        if (!layer.flow_poses.empty()) {
            std::cerr << "[PowerMenuScene] cache " << layer.name
                      << " flowPoses=" << layer.flow_poses.size()
                      << " macroPoses=0(analytic) combinedNodes="
                      << layer.flow_poses.size() << '\n';
        }
    }
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

    const PowerMenuFrame& frame = animator_->frame();
    const double viewport_scale = std::max(
        static_cast<double>(width) /
            static_cast<double>(manifest_->logical_canvas.width),
        static_cast<double>(height) /
            static_cast<double>(manifest_->logical_canvas.height)
    );
    const double offset_x = (
        static_cast<double>(width) -
        static_cast<double>(manifest_->logical_canvas.width) * viewport_scale
    ) * 0.5;
    const double offset_y = (
        static_cast<double>(height) -
        static_cast<double>(manifest_->logical_canvas.height) * viewport_scale
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
        static_cast<float>(viewport_scale),
        static_cast<float>(viewport_scale)
    );
    translation = GRAPHENE_POINT_INIT(
        static_cast<float>(manifest_->logical_canvas.width * 0.5),
        static_cast<float>(manifest_->logical_canvas.height * 0.5)
    );
    gtk_snapshot_translate(snapshot, &translation);
    gtk_snapshot_scale(
        snapshot,
        static_cast<float>(frame.scene_scale),
        static_cast<float>(frame.scene_scale)
    );
    translation = GRAPHENE_POINT_INIT(
        static_cast<float>(manifest_->logical_canvas.width * -0.5),
        static_cast<float>(manifest_->logical_canvas.height * -0.5)
    );
    gtk_snapshot_translate(snapshot, &translation);
    if (frame.scene_opacity < 0.9999) {
        gtk_snapshot_push_opacity(snapshot, static_cast<float>(frame.scene_opacity));
    }

    for (std::size_t index = 0; index < layers_.size(); ++index) {
        const auto& layer = layers_[index];
        const auto& motion = frame.layers[index];
        if (layer.animation.type == PowerMenuAnimationType::BlinkPatch) {
            const bool visible =
                (layer.animation.blink_state == "half" &&
                 frame.blink == PowerMenuBlinkState::Half) ||
                (layer.animation.blink_state == "closed" &&
                 frame.blink == PowerMenuBlinkState::Closed);
            if (!visible) continue;
        }

        GdkTexture* texture = layer.texture;
        if (!layer.flow_poses.empty()) {
            double fraction = 0.5;
            if (layer.animation.type == PowerMenuAnimationType::MeshFlow &&
                layer.flow_maximum > layer.flow_minimum) {
                fraction = (motion.flow_displacement - layer.flow_minimum) /
                    (layer.flow_maximum - layer.flow_minimum);
            } else if (layer.animation.type == PowerMenuAnimationType::FlowDrift) {
                fraction = (motion.flow_displacement + 1.0) * 0.5;
            }
            const auto pose_index = static_cast<std::size_t>(std::clamp(
                std::llround(std::clamp(fraction, 0.0, 1.0) *
                    static_cast<double>(layer.flow_poses.size() - 1U)),
                0LL,
                static_cast<long long>(layer.flow_poses.size() - 1U)
            ));
            texture = layer.flow_poses[pose_index];
        }

        if (layer.mesh && texture != nullptr) {
            const auto deformations = layer.mesh->pose(motion.macro_displacement);
            if (motion.opacity < 0.9999) {
                gtk_snapshot_push_opacity(snapshot, static_cast<float>(motion.opacity));
            }
            for (std::size_t strip_index = 0;
                 strip_index < layer.mesh->strips().size(); ++strip_index) {
                const auto& strip = layer.mesh->strips()[strip_index];
                const auto& deformation = deformations[strip_index];
                const double dx = (deformation.minimum_offset.x +
                    deformation.maximum_offset.x) * 0.5;
                const double dy = (deformation.minimum_offset.y +
                    deformation.maximum_offset.y) * 0.5;
                gtk_snapshot_save(snapshot);
                graphene_point_t strip_translation = GRAPHENE_POINT_INIT(
                    static_cast<float>(layer.placement.x + motion.translation_x + dx),
                    static_cast<float>(layer.placement.y + motion.translation_y + dy)
                );
                gtk_snapshot_translate(snapshot, &strip_translation);
                graphene_rect_t clip = GRAPHENE_RECT_INIT(
                    static_cast<float>(strip.x),
                    static_cast<float>(strip.y),
                    static_cast<float>(strip.width),
                    static_cast<float>(strip.height)
                );
                gtk_snapshot_push_clip(snapshot, &clip);
                graphene_rect_t bounds = GRAPHENE_RECT_INIT(
                    0.0F, 0.0F,
                    static_cast<float>(layer.placement.width),
                    static_cast<float>(layer.placement.height)
                );
                gtk_snapshot_append_scaled_texture(
                    snapshot, texture, GSK_SCALING_FILTER_LINEAR, &bounds
                );
                gtk_snapshot_pop(snapshot);
                gtk_snapshot_restore(snapshot);
            }
            if (motion.opacity < 0.9999) gtk_snapshot_pop(snapshot);
            continue;
        }

        const PowerMenuVector pivot = layer.animation.type == PowerMenuAnimationType::Spring
            ? layer.animation.pivot : PowerMenuVector{0.5, 0.5};
        append_texture(snapshot, texture, layer.placement, motion, pivot);
    }

    if (frame.scene_opacity < 0.9999) gtk_snapshot_pop(snapshot);
    gtk_snapshot_restore(snapshot);
}

void PowerMenuScene::ensure_tick() {
    if (tick_callback_id_ != 0 || widget_ == nullptr || !animator_ ||
        !animator_->needs_frame()) return;
    last_frame_time_us_ = 0;
    idle_accumulator_seconds_ = 0.0;
    tick_callback_id_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        16,
        &PowerMenuScene::timer_callback,
        this,
        nullptr
    );
}

void PowerMenuScene::stop_tick() {
    if (tick_callback_id_ != 0) g_source_remove(tick_callback_id_);
    tick_callback_id_ = 0;
    last_frame_time_us_ = 0;
    idle_accumulator_seconds_ = 0.0;
}

gboolean PowerMenuScene::on_timer() {
    if (!animator_ || !animator_->needs_frame()) {
        tick_callback_id_ = 0;
        last_frame_time_us_ = 0;
        auto callback = std::move(on_hidden_);
        if (callback) callback();
        return G_SOURCE_REMOVE;
    }

    const gint64 frame_time_us = g_get_monotonic_time();
    if (last_frame_time_us_ == 0) {
        last_frame_time_us_ = frame_time_us;
        return G_SOURCE_CONTINUE;
    }
    const double delta_seconds = std::clamp(
        static_cast<double>(frame_time_us - last_frame_time_us_) / 1'000'000.0,
        0.0, 0.10
    );
    last_frame_time_us_ = frame_time_us;

    const bool lifecycle = animator_->phase() == PowerMenuScenePhase::Opening ||
        animator_->phase() == PowerMenuScenePhase::Closing;
    idle_accumulator_seconds_ += delta_seconds;
    if (!lifecycle && idle_accumulator_seconds_ < kIdleFrameSeconds) {
        return G_SOURCE_CONTINUE;
    }
    const double advance_seconds = lifecycle
        ? delta_seconds : idle_accumulator_seconds_;
    idle_accumulator_seconds_ = 0.0;
    animator_->advance(advance_seconds);
    publish_visibility();
    gtk_widget_queue_draw(widget_);

    if (animator_->needs_frame()) return G_SOURCE_CONTINUE;
    tick_callback_id_ = 0;
    last_frame_time_us_ = 0;
    auto callback = std::move(on_hidden_);
    if (callback) callback();
    return G_SOURCE_REMOVE;
}

void PowerMenuScene::publish_visibility() {
    if (!visibility_callback_) return;
    visibility_callback_(animator_ ? animator_->frame().scene_opacity : 0.0);
}

void PowerMenuScene::release_layers() noexcept {
    for (auto& layer : layers_) {
        if (layer.texture != nullptr) g_object_unref(layer.texture);
        layer.texture = nullptr;
        for (GdkTexture*& pose : layer.flow_poses) {
            if (pose != nullptr) g_object_unref(pose);
            pose = nullptr;
        }
        layer.flow_poses.clear();
        layer.mesh.reset();
    }
    layers_.clear();
}

} // namespace realmheart::ui::powermenu
