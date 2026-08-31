#include "animation/character/CharacterCompositor.hpp"
#include "animation/character/HairFlowWarp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <unordered_set>
#include <utility>

namespace realmheart::animation::character {
namespace {

constexpr guint kIdleFrameIntervalMs = 66;
constexpr gint64 kLifecycleHairFrameIntervalUs = 33'000;

// Hair widgets used to cover the entire 726x sidebar surface. Invalidating
// those oversized transparent widgets forced GTK to revisit far more render
// area than the two hair textures occupy. These margins cover every authored
// lifecycle/idle displacement while keeping each animated widget tightly
// bounded around its actual pixels.
constexpr double kHairWidgetHorizontalPadding = 80.0;
constexpr double kHairWidgetVerticalPadding = 8.0;
constexpr double kStaticWidgetPadding = 3.0;

// Non-pinned lifecycle segments need enough room for the authored overshoot
// and retreat. Keeping this padding on the segment rather than every child
// leaves the opacity/translation node tightly bounded around real pixels.
constexpr double kMotionSegmentLeftPadding = 8.0;
constexpr double kMotionSegmentRightPadding = 58.0;
constexpr double kMotionSegmentVerticalPadding = 6.0;

// Lifecycle + frozen-idle exit deformation currently stays inside roughly
// -34..+14 logical pixels. A small safety margin keeps future tuning inside the
// immutable node cache without increasing per-frame work.
constexpr double kHairNodeMinOffset = -40.0;
constexpr double kHairNodeMaxOffset = 24.0;
constexpr double kHairNodeStep = 1.0;
constexpr double kFlowPoseMinDisplacement = -2.0;
constexpr double kFlowPoseMaxDisplacement = 2.0;
constexpr double kFlowPoseStep = 1.0;
// One-sixteenth of a rendered pixel is below the visible threshold for this
// slow secondary motion, while reducing idle invalidations from effectively
// every monitor frame to only the moments when a cached pose actually changes.
constexpr double kFlowNodeStep = 1.0 / 16.0;

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
constexpr GdkMemoryFormat kCairoArgb32MemoryFormat =
    GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
constexpr GdkMemoryFormat kCairoArgb32MemoryFormat =
    GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif

using SnapshotDrawFunc = void (*)(GtkWidget*, GtkSnapshot*, gpointer);

typedef struct _RealmheartSnapshotWidget {
    GtkWidget parent_instance;
    SnapshotDrawFunc draw_func;
    gpointer user_data;
} RealmheartSnapshotWidget;

typedef struct _RealmheartSnapshotWidgetClass {
    GtkWidgetClass parent_class;
} RealmheartSnapshotWidgetClass;

G_DEFINE_TYPE(
    RealmheartSnapshotWidget,
    realmheart_snapshot_widget,
    GTK_TYPE_WIDGET
)

void realmheart_snapshot_widget_snapshot(
    GtkWidget* widget,
    GtkSnapshot* snapshot
) {
    auto* self = reinterpret_cast<RealmheartSnapshotWidget*>(widget);
    if (self->draw_func != nullptr) {
        self->draw_func(widget, snapshot, self->user_data);
    }
}

void realmheart_snapshot_widget_class_init(
    RealmheartSnapshotWidgetClass* klass
) {
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = realmheart_snapshot_widget_snapshot;
    gtk_widget_class_set_css_name(
        widget_class,
        "realmheart-character-snapshot"
    );
}

void realmheart_snapshot_widget_init(RealmheartSnapshotWidget* self) {
    self->draw_func = nullptr;
    self->user_data = nullptr;
}

GtkWidget* snapshot_widget_new(
    SnapshotDrawFunc draw_func,
    gpointer user_data
) {
    auto* self = reinterpret_cast<RealmheartSnapshotWidget*>(
        g_object_new(realmheart_snapshot_widget_get_type(), nullptr)
    );
    self->draw_func = draw_func;
    self->user_data = user_data;
    return GTK_WIDGET(self);
}

void snapshot_widget_clear(GtkWidget* widget) {
    if (widget == nullptr) return;
    auto* self = reinterpret_cast<RealmheartSnapshotWidget*>(widget);
    self->draw_func = nullptr;
    self->user_data = nullptr;
}

typedef struct _RealmheartMotionWidget {
    GtkWidget parent_instance;
    GtkWidget* content;
    double offset_x;
    double offset_y;
    double opacity;
} RealmheartMotionWidget;

typedef struct _RealmheartMotionWidgetClass {
    GtkWidgetClass parent_class;
} RealmheartMotionWidgetClass;

G_DEFINE_TYPE(
    RealmheartMotionWidget,
    realmheart_motion_widget,
    GTK_TYPE_WIDGET
)

void realmheart_motion_widget_dispose(GObject* object) {
    auto* self = reinterpret_cast<RealmheartMotionWidget*>(object);
    if (self->content != nullptr) {
        gtk_widget_unparent(self->content);
        self->content = nullptr;
    }
    G_OBJECT_CLASS(realmheart_motion_widget_parent_class)->dispose(object);
}

void realmheart_motion_widget_measure(
    GtkWidget* widget,
    GtkOrientation orientation,
    int for_size,
    int* minimum,
    int* natural,
    int* minimum_baseline,
    int* natural_baseline
) {
    auto* self = reinterpret_cast<RealmheartMotionWidget*>(widget);
    if (self->content == nullptr) {
        *minimum = 0;
        *natural = 0;
        *minimum_baseline = -1;
        *natural_baseline = -1;
        return;
    }
    gtk_widget_measure(
        self->content,
        orientation,
        for_size,
        minimum,
        natural,
        minimum_baseline,
        natural_baseline
    );
}

void realmheart_motion_widget_size_allocate(
    GtkWidget* widget,
    int width,
    int height,
    int baseline
) {
    auto* self = reinterpret_cast<RealmheartMotionWidget*>(widget);
    if (self->content != nullptr) {
        gtk_widget_allocate(self->content, width, height, baseline, nullptr);
    }
}

void realmheart_motion_widget_snapshot(
    GtkWidget* widget,
    GtkSnapshot* snapshot
) {
    auto* self = reinterpret_cast<RealmheartMotionWidget*>(widget);
    if (self->content == nullptr || self->opacity <= 0.0001) return;

    graphene_point_t translation;
    graphene_point_init(
        &translation,
        static_cast<float>(self->offset_x),
        static_cast<float>(self->offset_y)
    );

    gtk_snapshot_save(snapshot);
    if (self->opacity < 0.9999) {
        gtk_snapshot_push_opacity(snapshot, self->opacity);
    }
    gtk_snapshot_translate(snapshot, &translation);
    gtk_widget_snapshot_child(widget, self->content, snapshot);
    if (self->opacity < 0.9999) gtk_snapshot_pop(snapshot);
    gtk_snapshot_restore(snapshot);
}

void realmheart_motion_widget_class_init(
    RealmheartMotionWidgetClass* klass
) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = realmheart_motion_widget_dispose;

    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->measure = realmheart_motion_widget_measure;
    widget_class->size_allocate = realmheart_motion_widget_size_allocate;
    widget_class->snapshot = realmheart_motion_widget_snapshot;
    gtk_widget_class_set_css_name(
        widget_class,
        "realmheart-character-motion"
    );
}

void realmheart_motion_widget_init(RealmheartMotionWidget* self) {
    self->content = gtk_fixed_new();
    self->offset_x = 0.0;
    self->offset_y = 0.0;
    self->opacity = 0.0;
    gtk_widget_set_parent(self->content, GTK_WIDGET(self));
}

GtkWidget* motion_widget_new() {
    return GTK_WIDGET(g_object_new(
        realmheart_motion_widget_get_type(),
        nullptr
    ));
}

GtkWidget* motion_widget_content(GtkWidget* widget) {
    if (widget == nullptr) return nullptr;
    auto* self = reinterpret_cast<RealmheartMotionWidget*>(widget);
    return self->content;
}

void motion_widget_set_state(
    GtkWidget* widget,
    double offset_x,
    double offset_y,
    double opacity
) {
    if (widget == nullptr) return;
    auto* self = reinterpret_cast<RealmheartMotionWidget*>(widget);
    constexpr double epsilon = 0.0001;
    const double clamped_opacity = std::clamp(opacity, 0.0, 1.0);
    if (std::abs(self->offset_x - offset_x) <= epsilon &&
        std::abs(self->offset_y - offset_y) <= epsilon &&
        std::abs(self->opacity - clamped_opacity) <= epsilon) {
        return;
    }

    self->offset_x = offset_x;
    self->offset_y = offset_y;
    self->opacity = clamped_opacity;
    gtk_widget_queue_draw(widget);
}

bool debug_enabled() {
    static const bool enabled = [] {
        const char* configured = std::getenv("REALMHEART_CHARACTER_DEBUG");
        return configured != nullptr && *configured != '\0' &&
            std::string_view(configured) != "0";
    }();
    return enabled;
}

bool pin_layer_during_exit(std::string_view layer_id) {
    // Tessia's side hand already appears to grip the sidebar edge. If it
    // retreats laterally during fade-out, it reads as though the hand slides
    // through the shell. Keep that layer spatially pinned while the rest of
    // the character exits behind the occluder.
    return layer_id == "side-hand";
}

} // namespace

void CharacterCompositor::SurfaceDeleter::operator()(cairo_surface_t* surface) const {
    if (surface != nullptr) cairo_surface_destroy(surface);
}

void CharacterCompositor::TextureDeleter::operator()(GdkTexture* texture) const {
    if (texture != nullptr) g_object_unref(texture);
}

void CharacterCompositor::RenderNodeDeleter::operator()(GskRenderNode* node) const {
    if (node != nullptr) gsk_render_node_unref(node);
}

std::unique_ptr<CharacterCompositor> CharacterCompositor::create(
    GtkWidget* back_host,
    GtkWidget* front_host,
    const std::filesystem::path& character_root,
    core::DisplayTier display_tier,
    CharacterHostGeometry host_geometry,
    std::string* error_message
) {
    if (back_host == nullptr || front_host == nullptr ||
        !GTK_IS_FIXED(back_host) || !GTK_IS_FIXED(front_host) ||
        host_geometry.surface_width <= 0 || host_geometry.surface_height <= 0) {
        if (error_message != nullptr) {
            *error_message = "Character compositor received invalid host layers or geometry";
        }
        return nullptr;
    }

    auto manifest = CharacterManifest::load(
        character_root, display_tier, error_message
    );
    if (!manifest) return nullptr;

    auto compositor = std::unique_ptr<CharacterCompositor>(
        new CharacterCompositor(
            back_host,
            front_host,
            std::move(*manifest),
            host_geometry
        )
    );
    if (!compositor->load_surfaces(error_message) ||
        !compositor->build_hair_meshes(error_message) ||
        !compositor->create_draw_groups(error_message)) {
        return nullptr;
    }
    return compositor;
}

CharacterCompositor::CharacterCompositor(
    GtkWidget* back_host,
    GtkWidget* front_host,
    CharacterManifest manifest,
    CharacterHostGeometry host_geometry
) : back_host_(back_host),
    front_host_(front_host),
    manifest_(std::move(manifest)),
    host_geometry_(host_geometry) {}

CharacterCompositor::~CharacterCompositor() {
    stop_tick();
    stop_idle_timeout(true);
    for (auto& group : draw_groups_) {
        if (group->widget == nullptr) continue;
        if (group->kind == DrawGroupKind::Hair) {
            snapshot_widget_clear(group->widget);
        } else {
            gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(group->widget), nullptr, nullptr, nullptr
            );
        }
        GtkWidget* parent = gtk_widget_get_parent(group->widget);
        if (parent != nullptr && GTK_IS_FIXED(parent)) {
            gtk_fixed_remove(GTK_FIXED(parent), group->widget);
        }
        group->widget = nullptr;
    }
    draw_groups_.clear();

    for (auto& segment : motion_segments_) {
        if (segment->widget == nullptr) continue;
        GtkWidget* host = host_for_plane(segment->plane);
        if (host != nullptr && gtk_widget_get_parent(segment->widget) == host) {
            gtk_fixed_remove(GTK_FIXED(host), segment->widget);
        }
        segment->widget = nullptr;
        segment->content = nullptr;
    }
    motion_segments_.clear();
    tick_widget_ = nullptr;
}

bool CharacterCompositor::create_draw_groups(std::string* error_message) {
    DrawGroup* last_back = nullptr;
    DrawGroup* last_front = nullptr;

    // First construct the logical z-ordered groups. Widget creation happens in
    // a second pass because a group's tight bounds are only known after all
    // consecutive layers belonging to it have been collected.
    for (std::size_t index = 0; index < manifest_.layers.size(); ++index) {
        const CharacterLayer& layer = manifest_.layers[index];
        if (!layer.visible) continue;

        const DrawGroupKind kind = classify_layer(layer);
        const bool pin_during_exit = pin_layer_during_exit(layer.id);
        DrawGroup*& previous = layer.plane == CharacterPlane::Back
            ? last_back
            : last_front;
        if (previous != nullptr && previous->kind == kind &&
            previous->pin_during_exit == pin_during_exit) {
            previous->layer_indices.push_back(index);
            continue;
        }

        auto group = std::make_unique<DrawGroup>();
        group->owner = this;
        group->plane = layer.plane;
        group->kind = kind;
        group->pin_during_exit = pin_during_exit;
        group->layer_indices.push_back(index);
        previous = group.get();
        draw_groups_.push_back(std::move(group));
    }

    if (draw_groups_.empty()) {
        if (error_message != nullptr) {
            *error_message = "Character manifest has no visible drawable layers";
        }
        return false;
    }

    const double target_height =
        static_cast<double>(host_geometry_.surface_height) *
        manifest_.placement.height_fraction;
    const double scale = manifest_.source_canvas.height > 0
        ? target_height / static_cast<double>(manifest_.source_canvas.height)
        : 0.0;
    const double source_anchor_x = manifest_.placement.source_anchor.x *
        static_cast<double>(manifest_.source_canvas.width);
    const double source_anchor_y = manifest_.placement.source_anchor.y *
        static_cast<double>(manifest_.source_canvas.height);
    const double character_origin_x = host_geometry_.occlusion_left +
        manifest_.placement.host_offset.x - (source_anchor_x * scale);
    const double character_origin_y = host_geometry_.occlusion_top +
        manifest_.placement.host_offset.y - (source_anchor_y * scale);

    MotionSegment* last_back_segment = nullptr;
    MotionSegment* last_front_segment = nullptr;

    for (auto& group : draw_groups_) {
        GtkWidget* host = host_for_plane(group->plane);
        if (host == nullptr || !GTK_IS_FIXED(host)) {
            if (error_message != nullptr) {
                *error_message = "Character draw group received an invalid host";
            }
            return false;
        }

        group->host_x = 0.0;
        group->host_y = 0.0;
        group->width = host_geometry_.surface_width;
        group->height = host_geometry_.surface_height;

        if (group->kind == DrawGroupKind::Hair) {
            if (!std::isfinite(scale) || scale <= 0.0) {
                if (error_message != nullptr) {
                    *error_message = "Unable to derive tight hair widget geometry";
                }
                return false;
            }

            double left = std::numeric_limits<double>::infinity();
            double top = std::numeric_limits<double>::infinity();
            double right = -std::numeric_limits<double>::infinity();
            double bottom = -std::numeric_limits<double>::infinity();

            for (const std::size_t layer_index : group->layer_indices) {
                if (layer_index >= manifest_.layers.size()) continue;
                const CharacterLayer& layer = manifest_.layers[layer_index];
                const CharacterAsset* asset = manifest_.find_asset(layer.asset_id);
                const auto cache = hair_render_caches_.find(layer.id);
                if (asset == nullptr || cache == hair_render_caches_.end()) {
                    if (error_message != nullptr) {
                        *error_message = "Unable to find tight bounds for hair layer " +
                            layer.id;
                    }
                    return false;
                }

                double x = 0.0;
                double y = 0.0;
                if (layer.placement == CharacterLayerPlacement::SourceCanvas) {
                    x = character_origin_x + (asset->offset.x * scale);
                    y = character_origin_y + (asset->offset.y * scale);
                } else {
                    x = host_geometry_.occlusion_left + layer.host_offset.x;
                    y = host_geometry_.occlusion_top + layer.host_offset.y;
                }

                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(
                    right,
                    x + static_cast<double>(cache->second.width)
                );
                bottom = std::max(
                    bottom,
                    y + static_cast<double>(cache->second.height)
                );
            }

            if (!std::isfinite(left) || !std::isfinite(top) ||
                !std::isfinite(right) || !std::isfinite(bottom)) {
                if (error_message != nullptr) {
                    *error_message = "Hair draw group produced invalid bounds";
                }
                return false;
            }

            const double bounded_left = std::clamp(
                std::floor(left - kHairWidgetHorizontalPadding),
                0.0,
                static_cast<double>(host_geometry_.surface_width - 1)
            );
            const double bounded_top = std::clamp(
                std::floor(top - kHairWidgetVerticalPadding),
                0.0,
                static_cast<double>(host_geometry_.surface_height - 1)
            );
            const double bounded_right = std::clamp(
                std::ceil(right + kHairWidgetHorizontalPadding),
                bounded_left + 1.0,
                static_cast<double>(host_geometry_.surface_width)
            );
            const double bounded_bottom = std::clamp(
                std::ceil(bottom + kHairWidgetVerticalPadding),
                bounded_top + 1.0,
                static_cast<double>(host_geometry_.surface_height)
            );

            group->host_x = bounded_left;
            group->host_y = bounded_top;
            group->width = std::max(
                static_cast<int>(std::ceil(bounded_right - bounded_left)),
                1
            );
            group->height = std::max(
                static_cast<int>(std::ceil(bounded_bottom - bounded_top)),
                1
            );

            group->widget = snapshot_widget_new(
                &CharacterCompositor::snapshot_callback,
                group.get()
            );
            gtk_widget_set_size_request(
                group->widget,
                group->width,
                group->height
            );
        } else {
            double left = std::numeric_limits<double>::infinity();
            double top = std::numeric_limits<double>::infinity();
            double right = -std::numeric_limits<double>::infinity();
            double bottom = -std::numeric_limits<double>::infinity();

            const auto include_asset_bounds = [&](const CharacterLayer& layer,
                                                  const std::string& asset_id) {
                const CharacterAsset* asset = manifest_.find_asset(asset_id);
                if (asset == nullptr) return;

                double x = 0.0;
                double y = 0.0;
                if (layer.placement == CharacterLayerPlacement::SourceCanvas) {
                    x = character_origin_x + (asset->offset.x * scale);
                    y = character_origin_y + (asset->offset.y * scale);
                } else {
                    x = host_geometry_.occlusion_left + layer.host_offset.x;
                    y = host_geometry_.occlusion_top + layer.host_offset.y;
                }

                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(
                    right,
                    x + (static_cast<double>(asset->size.width) * scale)
                );
                bottom = std::max(
                    bottom,
                    y + (static_cast<double>(asset->size.height) * scale)
                );
            };

            for (const std::size_t layer_index : group->layer_indices) {
                if (layer_index >= manifest_.layers.size()) continue;
                const CharacterLayer& layer = manifest_.layers[layer_index];
                include_asset_bounds(layer, layer.asset_id);

                if (!manifest_.expression.enabled) continue;
                const auto& expression = manifest_.expression;
                if (layer.id == expression.eyes_layer_id) {
                    include_asset_bounds(layer, expression.eyes_inward_asset_id);
                    include_asset_bounds(layer, expression.eyes_half_asset_id);
                    include_asset_bounds(layer, expression.eyes_closed_asset_id);
                } else if (layer.id == expression.mouth_layer_id) {
                    include_asset_bounds(layer, expression.mouth_curious_asset_id);
                }
            }

            if (!std::isfinite(left) || !std::isfinite(top) ||
                !std::isfinite(right) || !std::isfinite(bottom)) {
                if (error_message != nullptr) {
                    *error_message = "Static character draw group produced invalid bounds";
                }
                return false;
            }

            const double bounded_left = std::clamp(
                std::floor(left - kStaticWidgetPadding),
                0.0,
                static_cast<double>(host_geometry_.surface_width - 1)
            );
            const double bounded_top = std::clamp(
                std::floor(top - kStaticWidgetPadding),
                0.0,
                static_cast<double>(host_geometry_.surface_height - 1)
            );
            const double bounded_right = std::clamp(
                std::ceil(right + kStaticWidgetPadding),
                bounded_left + 1.0,
                static_cast<double>(host_geometry_.surface_width)
            );
            const double bounded_bottom = std::clamp(
                std::ceil(bottom + kStaticWidgetPadding),
                bounded_top + 1.0,
                static_cast<double>(host_geometry_.surface_height)
            );

            group->host_x = bounded_left;
            group->host_y = bounded_top;
            group->width = std::max(
                static_cast<int>(std::ceil(bounded_right - bounded_left)),
                1
            );
            group->height = std::max(
                static_cast<int>(std::ceil(bounded_bottom - bounded_top)),
                1
            );

            group->widget = gtk_drawing_area_new();
            gtk_drawing_area_set_content_width(
                GTK_DRAWING_AREA(group->widget), group->width
            );
            gtk_drawing_area_set_content_height(
                GTK_DRAWING_AREA(group->widget), group->height
            );
            gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(group->widget),
                &CharacterCompositor::draw_callback,
                group.get(),
                nullptr
            );
        }

        gtk_widget_set_can_target(group->widget, FALSE);
        gtk_widget_set_focusable(group->widget, FALSE);
        gtk_widget_set_overflow(group->widget, GTK_OVERFLOW_HIDDEN);

        MotionSegment*& last_segment = group->plane == CharacterPlane::Back
            ? last_back_segment
            : last_front_segment;
        if (last_segment == nullptr ||
            last_segment->pin_during_exit != group->pin_during_exit) {
            auto segment = std::make_unique<MotionSegment>();
            segment->plane = group->plane;
            segment->pin_during_exit = group->pin_during_exit;
            segment->widget = motion_widget_new();
            segment->content = motion_widget_content(segment->widget);
            if (segment->widget == nullptr || segment->content == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Unable to create character motion segment";
                }
                return false;
            }

            gtk_widget_set_can_target(segment->widget, FALSE);
            gtk_widget_set_focusable(segment->widget, FALSE);
            gtk_widget_set_overflow(segment->widget, GTK_OVERFLOW_HIDDEN);

            last_segment = segment.get();
            motion_segments_.push_back(std::move(segment));
            if (tick_widget_ == nullptr) tick_widget_ = last_segment->widget;
        }

        last_segment->groups.push_back(group.get());
    }

    for (auto& segment : motion_segments_) {
        if (segment->groups.empty()) continue;

        double left = std::numeric_limits<double>::infinity();
        double top = std::numeric_limits<double>::infinity();
        double right = -std::numeric_limits<double>::infinity();
        double bottom = -std::numeric_limits<double>::infinity();
        for (const DrawGroup* group : segment->groups) {
            if (group == nullptr) continue;
            left = std::min(left, group->host_x);
            top = std::min(top, group->host_y);
            right = std::max(
                right,
                group->host_x + static_cast<double>(group->width)
            );
            bottom = std::max(
                bottom,
                group->host_y + static_cast<double>(group->height)
            );
        }

        if (!std::isfinite(left) || !std::isfinite(top) ||
            !std::isfinite(right) || !std::isfinite(bottom)) {
            if (error_message != nullptr) {
                *error_message = "Character motion segment produced invalid bounds";
            }
            return false;
        }

        const double left_padding = segment->pin_during_exit
            ? 0.0
            : kMotionSegmentLeftPadding;
        const double right_padding = segment->pin_during_exit
            ? 0.0
            : kMotionSegmentRightPadding;
        const double vertical_padding = segment->pin_during_exit
            ? 0.0
            : kMotionSegmentVerticalPadding;
        const double bounded_left = std::clamp(
            std::floor(left - left_padding),
            0.0,
            static_cast<double>(host_geometry_.surface_width - 1)
        );
        const double bounded_top = std::clamp(
            std::floor(top - vertical_padding),
            0.0,
            static_cast<double>(host_geometry_.surface_height - 1)
        );
        const double bounded_right = std::clamp(
            std::ceil(right + right_padding),
            bounded_left + 1.0,
            static_cast<double>(host_geometry_.surface_width)
        );
        const double bounded_bottom = std::clamp(
            std::ceil(bottom + vertical_padding),
            bounded_top + 1.0,
            static_cast<double>(host_geometry_.surface_height)
        );

        segment->host_x = bounded_left;
        segment->host_y = bounded_top;
        segment->width = std::max(
            static_cast<int>(std::ceil(bounded_right - bounded_left)),
            1
        );
        segment->height = std::max(
            static_cast<int>(std::ceil(bounded_bottom - bounded_top)),
            1
        );

        gtk_widget_set_size_request(
            segment->widget,
            segment->width,
            segment->height
        );
        GtkWidget* host = host_for_plane(segment->plane);
        gtk_fixed_put(
            GTK_FIXED(host),
            segment->widget,
            segment->host_x,
            segment->host_y
        );

        for (DrawGroup* group : segment->groups) {
            if (group == nullptr || group->widget == nullptr) continue;
            gtk_fixed_put(
                GTK_FIXED(segment->content),
                group->widget,
                group->host_x - segment->host_x,
                group->host_y - segment->host_y
            );
        }
    }

    for (auto iterator = draw_groups_.rbegin(); iterator != draw_groups_.rend(); ++iterator) {
        if ((*iterator)->plane == CharacterPlane::Front) {
            (*iterator)->draw_debug_anchor = true;
            break;
        }
    }
    return true;
}

CharacterCompositor::DrawGroupKind CharacterCompositor::classify_layer(
    const CharacterLayer& layer
) const noexcept {
    if (layer.renderer == CharacterLayerRenderer::HairMesh) {
        return DrawGroupKind::Hair;
    }
    if (is_expression_layer(layer)) return DrawGroupKind::Expression;
    return DrawGroupKind::Static;
}

bool CharacterCompositor::is_expression_layer(
    const CharacterLayer& layer
) const noexcept {
    return manifest_.expression.enabled &&
        (layer.id == manifest_.expression.eyes_layer_id ||
         layer.id == manifest_.expression.mouth_layer_id);
}

GtkWidget* CharacterCompositor::host_for_plane(CharacterPlane plane) const noexcept {
    return plane == CharacterPlane::Back ? back_host_ : front_host_;
}

bool CharacterCompositor::has_group(DrawGroupKind kind) const noexcept {
    return std::any_of(
        draw_groups_.begin(),
        draw_groups_.end(),
        [kind](const auto& group) { return group->kind == kind; }
    );
}

bool CharacterCompositor::set_hair_mode(
    CharacterHairMode mode,
    std::string* error_message
) {
    if (mode == CharacterHairMode::MeshFlow && !flow_caches_loaded_) {
        if (!ensure_flow_caches(error_message)) return false;
        flow_elapsed_seconds_ = 0.0;
    }

    if (hair_mode_ == mode) return true;
    hair_mode_ = mode;

    if (hair_mode_ != CharacterHairMode::MeshFlow && flow_caches_loaded_) {
        release_flow_caches();
    }

    for (const auto& group : draw_groups_) {
        group->has_hair_pose_signature = false;
    }
    queue_hair_draw(true);

    if (flow_tick_active()) {
        ensure_tick();
    } else if (!animator_.active()) {
        stop_tick();
    }
    if (animator_.idle()) ensure_idle_timeout();
    return true;
}

void CharacterCompositor::start_enter() {
    if (animator_.idle()) return;

    const bool was_hidden = animator_.hidden();
    stop_idle_timeout(true);
    if (manifest_.expression.enabled) {
        if (was_hidden) {
            expression_animator_.reset_base();
        } else {
            expression_animator_.pause_stable();
        }
    }
    if (was_hidden) flow_elapsed_seconds_ = 0.0;
    animator_.start_enter();
    last_hair_draw_time_us_ = 0;
    apply_motion_state();
    queue_draw();
    ensure_tick();
}

void CharacterCompositor::start_exit() {
    // Preserve the last idle deformation during exit so the hair does not
    // snap back to its authored geometry before the retreat begins.
    stop_idle_timeout(false);
    if (manifest_.expression.enabled) expression_animator_.pause_stable();
    animator_.start_exit();
    last_hair_draw_time_us_ = 0;
    apply_motion_state();
    queue_draw();
    ensure_tick();
}

gboolean CharacterCompositor::tick_callback(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer raw
) {
    auto* compositor = static_cast<CharacterCompositor*>(raw);
    return compositor != nullptr
        ? compositor->on_tick(frame_clock)
        : G_SOURCE_REMOVE;
}

gboolean CharacterCompositor::idle_timeout_callback(gpointer raw) {
    auto* compositor = static_cast<CharacterCompositor*>(raw);
    return compositor != nullptr
        ? compositor->on_idle_timeout()
        : G_SOURCE_REMOVE;
}

void CharacterCompositor::apply_motion_state() {
    const auto& animation = animator_.sample();
    const double opacity = animator_.display_opacity();
    for (const auto& segment : motion_segments_) {
        const bool pinned =
            animator_.phase() == CharacterAnimationPhase::Exiting &&
            segment->pin_during_exit;
        motion_widget_set_state(
            segment->widget,
            pinned ? 0.0 : animation.offset_x,
            pinned ? 0.0 : animation.offset_y,
            opacity
        );
    }
}

void CharacterCompositor::ensure_tick() {
    if (tick_callback_id_ != 0 || tick_widget_ == nullptr ||
        (!animator_.active() && !flow_tick_active())) {
        return;
    }

    last_frame_time_us_ = 0;
    tick_callback_id_ = gtk_widget_add_tick_callback(
        tick_widget_,
        &CharacterCompositor::tick_callback,
        this,
        nullptr
    );
}

void CharacterCompositor::stop_tick() {
    if (tick_callback_id_ == 0 || tick_widget_ == nullptr) return;
    gtk_widget_remove_tick_callback(tick_widget_, tick_callback_id_);
    tick_callback_id_ = 0;
    last_frame_time_us_ = 0;
    last_hair_draw_time_us_ = 0;
}

void CharacterCompositor::ensure_idle_timeout() {
    const bool has_idle_hair = has_group(DrawGroupKind::Hair);
    const bool has_active_expression =
        manifest_.expression.enabled && expression_animator_.active();
    if (idle_timeout_id_ != 0 || !animator_.idle() ||
        (!has_idle_hair && !has_active_expression)) {
        return;
    }

    last_idle_time_us_ = g_get_monotonic_time();
    idle_timeout_id_ = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        kIdleFrameIntervalMs,
        &CharacterCompositor::idle_timeout_callback,
        this,
        nullptr
    );
}

void CharacterCompositor::stop_idle_timeout(bool reset_elapsed) {
    if (idle_timeout_id_ != 0) {
        g_source_remove(idle_timeout_id_);
        idle_timeout_id_ = 0;
    }
    last_idle_time_us_ = 0;
    if (reset_elapsed) idle_elapsed_seconds_ = 0.0;
}

gboolean CharacterCompositor::on_tick(GdkFrameClock* frame_clock) {
    if (frame_clock == nullptr ||
        (!animator_.active() && !flow_tick_active())) {
        tick_callback_id_ = 0;
        last_frame_time_us_ = 0;
        return G_SOURCE_REMOVE;
    }

    const gint64 frame_time_us = gdk_frame_clock_get_frame_time(frame_clock);
    if (last_frame_time_us_ == 0) {
        last_frame_time_us_ = frame_time_us;
        return G_SOURCE_CONTINUE;
    }

    const double delta_seconds = std::clamp(
        static_cast<double>(frame_time_us - last_frame_time_us_) / 1'000'000.0,
        0.0,
        0.10
    );
    last_frame_time_us_ = frame_time_us;
    if (hair_mode_ == CharacterHairMode::MeshFlow && !animator_.hidden()) {
        flow_elapsed_seconds_ += delta_seconds;
    }

    if (animator_.active() && animator_.advance(delta_seconds)) {
        apply_motion_state();
        if (hair_mode_ == CharacterHairMode::MeshFlow) {
            queue_hair_draw();
        } else {
            queue_lifecycle_draw(frame_time_us);
        }
    } else if (flow_tick_active()) {
        queue_hair_draw();
    }

    if (animator_.active() || flow_tick_active()) {
        if (animator_.idle()) {
            if (manifest_.expression.enabled) expression_animator_.resume();
            ensure_idle_timeout();
        }
        return G_SOURCE_CONTINUE;
    }

    tick_callback_id_ = 0;
    last_frame_time_us_ = 0;
    last_hair_draw_time_us_ = 0;
    if (animator_.idle()) {
        if (manifest_.expression.enabled) expression_animator_.resume();
        ensure_idle_timeout();
    }
    return G_SOURCE_REMOVE;
}

gboolean CharacterCompositor::on_idle_timeout() {
    if (!animator_.idle()) {
        idle_timeout_id_ = 0;
        last_idle_time_us_ = 0;
        return G_SOURCE_REMOVE;
    }

    const gint64 now_us = g_get_monotonic_time();
    if (last_idle_time_us_ == 0) {
        last_idle_time_us_ = now_us;
        return G_SOURCE_CONTINUE;
    }

    const double delta_seconds = std::clamp(
        static_cast<double>(now_us - last_idle_time_us_) / 1'000'000.0,
        0.0,
        0.10
    );
    last_idle_time_us_ = now_us;
    idle_elapsed_seconds_ += delta_seconds;
    if (hair_mode_ != CharacterHairMode::Static) queue_hair_draw();
    if (manifest_.expression.enabled &&
        expression_animator_.advance(delta_seconds)) {
        queue_expression_draw();
    }
    return G_SOURCE_CONTINUE;
}

double CharacterCompositor::idle_hair_offset(
    const CharacterLayer& layer
) const noexcept {
    if (hair_mode_ == CharacterHairMode::Static ||
        layer.renderer != CharacterLayerRenderer::HairMesh ||
        layer.idle_strength <= 0.0 ||
        (!animator_.idle() &&
         animator_.phase() != CharacterAnimationPhase::Exiting)) {
        return 0.0;
    }

    return CharacterAnimator::idle_hair_wave(
        idle_elapsed_seconds_,
        layer.idle_phase
    ) * layer.idle_strength;
}

double CharacterCompositor::flow_hair_displacement(
    const CharacterLayer& layer
) const noexcept {
    if (hair_mode_ != CharacterHairMode::MeshFlow ||
        layer.flow_asset_id.empty() || layer.flow_strength <= 0.0 ||
        animator_.hidden()) {
        return 0.0;
    }

    const double primary = std::sin(
        (flow_elapsed_seconds_ * layer.flow_frequency) + layer.flow_phase
    );
    const double secondary = std::sin(
        (flow_elapsed_seconds_ * layer.flow_frequency * 1.61) +
        (layer.flow_phase * 0.73) + 0.47
    );
    return layer.flow_strength * ((0.78 * primary) + (0.22 * secondary));
}

bool CharacterCompositor::flow_tick_active() const noexcept {
    if (hair_mode_ != CharacterHairMode::MeshFlow || animator_.hidden()) {
        return false;
    }
    return std::any_of(
        manifest_.layers.begin(),
        manifest_.layers.end(),
        [this](const CharacterLayer& layer) {
            if (!layer.visible || layer.flow_asset_id.empty()) return false;
            const auto cache = hair_render_caches_.find(layer.id);
            return cache != hair_render_caches_.end() &&
                !cache->second.flow_poses.empty();
        }
    );
}

std::size_t CharacterCompositor::hair_pose_signature(
    const DrawGroup& group
) const noexcept {
    const auto& animation = animator_.sample();
    std::size_t signature = 0xcbf29ce484222325ULL;
    bool found_hair = false;

    for (const std::size_t layer_index : group.layer_indices) {
        if (layer_index >= manifest_.layers.size()) continue;
        const CharacterLayer& layer = manifest_.layers[layer_index];
        if (!layer.visible ||
            layer.renderer != CharacterLayerRenderer::HairMesh) {
            continue;
        }

        const auto cache = hair_render_caches_.find(layer.id);
        if (cache == hair_render_caches_.end()) continue;

        const double tip_offset_logical_pixels =
            hair_mode_ == CharacterHairMode::Static
                ? 0.0
                : (animation.hair_tip_offset_x * layer.mesh_strength) +
                    idle_hair_offset(layer);
        const std::size_t pose_index = quantized_hair_node_index(
            cache->second,
            tip_offset_logical_pixels
        );
        signature ^= pose_index + (layer_index * 0x9e3779b9U);
        signature *= 0x100000001b3ULL;

        if (hair_mode_ == CharacterHairMode::MeshFlow &&
            !cache->second.flow_quantized_nodes.empty() &&
            cache->second.flow_node_step > 0.0) {
            const std::size_t flow_index = quantized_flow_node_index(
                cache->second,
                flow_hair_displacement(layer)
            );
            signature ^= (flow_index << 8U) ^ 0x6a09e667U;
            signature *= 0x100000001b3ULL;
        }
        found_hair = true;
    }

    return found_hair ? signature : 0;
}

void CharacterCompositor::queue_hair_draw(bool force) {
    for (const auto& group : draw_groups_) {
        if (group->kind != DrawGroupKind::Hair || group->widget == nullptr) {
            continue;
        }

        const std::size_t signature = hair_pose_signature(*group);
        if (!force && group->has_hair_pose_signature &&
            group->last_hair_pose_signature == signature) {
            continue;
        }

        group->last_hair_pose_signature = signature;
        group->has_hair_pose_signature = true;
        gtk_widget_queue_draw(group->widget);
    }
}

void CharacterCompositor::queue_lifecycle_draw(gint64 frame_time_us) {
    const bool hair_due = last_hair_draw_time_us_ == 0 ||
        frame_time_us - last_hair_draw_time_us_ >=
            kLifecycleHairFrameIntervalUs ||
        !animator_.active();
    if (!hair_due) return;

    // Static body, expression and hand groups retain their existing render
    // nodes during lifecycle motion. Their widgets are translated and faded
    // compositor-side, so only deformation-dependent hair content must be
    // invalidated here.
    queue_hair_draw();
    last_hair_draw_time_us_ = frame_time_us;
}

void CharacterCompositor::queue_expression_draw() {
    for (const auto& group : draw_groups_) {
        if (group->kind == DrawGroupKind::Expression && group->widget != nullptr) {
            gtk_widget_queue_draw(group->widget);
        }
    }
}

const std::string* CharacterCompositor::selected_asset_id(
    const CharacterLayer& layer
) const noexcept {
    if (!manifest_.expression.enabled) return &layer.asset_id;

    const auto& rig = manifest_.expression;
    const auto& expression = expression_animator_.sample();
    if (layer.id == rig.eyes_layer_id) {
        switch (expression.eyes) {
        case CharacterEyeFrame::Base:
            return nullptr;
        case CharacterEyeFrame::Inward:
            return &rig.eyes_inward_asset_id;
        case CharacterEyeFrame::Half:
            return &rig.eyes_half_asset_id;
        case CharacterEyeFrame::Closed:
            return &rig.eyes_closed_asset_id;
        }
    }
    if (layer.id == rig.mouth_layer_id) {
        return expression.mouth == CharacterMouthFrame::Base
            ? nullptr
            : &rig.mouth_curious_asset_id;
    }
    return &layer.asset_id;
}

bool CharacterCompositor::load_surfaces(std::string* error_message) {
    std::unordered_set<std::string> required_assets;
    for (const auto& layer : manifest_.layers) {
        if (layer.visible) required_assets.insert(layer.asset_id);
    }
    if (manifest_.expression.enabled) {
        required_assets.insert(manifest_.expression.eyes_inward_asset_id);
        required_assets.insert(manifest_.expression.eyes_half_asset_id);
        required_assets.insert(manifest_.expression.eyes_closed_asset_id);
        required_assets.insert(manifest_.expression.mouth_curious_asset_id);
    }

    for (const auto& asset_id : required_assets) {
        const auto* asset = manifest_.find_asset(asset_id);
        if (asset == nullptr) continue;

        SurfacePtr surface(
            cairo_image_surface_create_from_png(asset->path.c_str())
        );
        const cairo_status_t status = cairo_surface_status(surface.get());
        if (status != CAIRO_STATUS_SUCCESS) {
            if (error_message != nullptr) {
                *error_message = "Unable to decode character asset " +
                    asset->path.string() + ": " + cairo_status_to_string(status);
            }
            return false;
        }

        if (cairo_image_surface_get_width(surface.get()) != asset->size.width ||
            cairo_image_surface_get_height(surface.get()) != asset->size.height) {
            if (error_message != nullptr) {
                *error_message = "Decoded character asset dimensions disagree with manifest: " +
                    asset->path.string();
            }
            return false;
        }
        surfaces_.emplace(asset_id, std::move(surface));
    }
    return true;
}

bool CharacterCompositor::build_hair_meshes(std::string* error_message) {
    if (manifest_.source_canvas.height <= 0 || host_geometry_.surface_height <= 0) {
        if (error_message != nullptr) {
            *error_message = "Unable to derive final hair render scale";
        }
        return false;
    }

    const double target_height = static_cast<double>(host_geometry_.surface_height) *
        manifest_.placement.height_fraction;
    const double render_scale = target_height /
        static_cast<double>(manifest_.source_canvas.height);
    if (!std::isfinite(render_scale) || render_scale <= 0.0) {
        if (error_message != nullptr) {
            *error_message = "Character hair render scale is invalid";
        }
        return false;
    }

    std::unordered_set<std::string> cached_source_assets;
    for (const auto& layer : manifest_.layers) {
        if (!layer.visible || layer.renderer != CharacterLayerRenderer::HairMesh) {
            continue;
        }

        const CharacterAsset* mask_asset = manifest_.find_asset(
            layer.mask_asset_id
        );
        const CharacterAsset* hair_asset = manifest_.find_asset(layer.asset_id);
        const auto hair_surface = surfaces_.find(layer.asset_id);
        if (mask_asset == nullptr || hair_asset == nullptr ||
            hair_surface == surfaces_.end()) {
            if (error_message != nullptr) {
                *error_message = "Missing assets for mesh layer " + layer.id;
            }
            return false;
        }

        SurfacePtr mask_surface(
            cairo_image_surface_create_from_png(mask_asset->path.c_str())
        );
        const cairo_status_t mask_status = cairo_surface_status(mask_surface.get());
        if (mask_status != CAIRO_STATUS_SUCCESS) {
            if (error_message != nullptr) {
                *error_message = "Unable to decode movement mask " +
                    mask_asset->path.string() + ": " +
                    cairo_status_to_string(mask_status);
            }
            return false;
        }
        if (cairo_image_surface_get_format(mask_surface.get()) != CAIRO_FORMAT_ARGB32 ||
            cairo_image_surface_get_width(mask_surface.get()) != mask_asset->size.width ||
            cairo_image_surface_get_height(mask_surface.get()) != mask_asset->size.height) {
            if (error_message != nullptr) {
                *error_message = "Movement mask must decode as ARGB32 with manifest geometry: " +
                    mask_asset->path.string();
            }
            return false;
        }

        cairo_surface_flush(mask_surface.get());
        auto mesh = HairMesh::from_argb32(
            cairo_image_surface_get_data(mask_surface.get()),
            cairo_image_surface_get_width(mask_surface.get()),
            cairo_image_surface_get_height(mask_surface.get()),
            cairo_image_surface_get_stride(mask_surface.get()),
            layer.mesh_rows,
            error_message
        );
        if (!mesh) return false;

        HairRenderCache cache;
        cache.width = std::max(
            static_cast<int>(std::ceil(
                static_cast<double>(hair_asset->size.width) * render_scale
            )),
            1
        );
        cache.height = std::max(
            static_cast<int>(std::ceil(
                static_cast<double>(hair_asset->size.height) * render_scale
            )),
            1
        );
        SurfacePtr scaled_surface(cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32,
            cache.width,
            cache.height
        ));
        const cairo_status_t cache_status = cairo_surface_status(
            scaled_surface.get()
        );
        if (cache_status != CAIRO_STATUS_SUCCESS) {
            if (error_message != nullptr) {
                *error_message = "Unable to allocate scaled hair cache for " +
                    layer.id + ": " + cairo_status_to_string(cache_status);
            }
            return false;
        }

        cairo_t* cache_cr = cairo_create(scaled_surface.get());
        if (cache_cr == nullptr || cairo_status(cache_cr) != CAIRO_STATUS_SUCCESS) {
            if (error_message != nullptr) {
                *error_message = "Unable to create scaled hair cache context for " +
                    layer.id;
            }
            if (cache_cr != nullptr) cairo_destroy(cache_cr);
            return false;
        }
        cairo_set_operator(cache_cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cache_cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cache_cr);
        cairo_scale(cache_cr, render_scale, render_scale);
        cairo_set_source_surface(cache_cr, hair_surface->second.get(), 0.0, 0.0);
        if (cairo_pattern_t* source = cairo_get_source(cache_cr); source != nullptr) {
            cairo_pattern_set_filter(source, CAIRO_FILTER_BILINEAR);
            cairo_pattern_set_extend(source, CAIRO_EXTEND_NONE);
        }
        cairo_paint(cache_cr);
        const cairo_status_t paint_status = cairo_status(cache_cr);
        cairo_destroy(cache_cr);
        if (paint_status != CAIRO_STATUS_SUCCESS) {
            if (error_message != nullptr) {
                *error_message = "Unable to populate scaled hair cache for " +
                    layer.id + ": " + cairo_status_to_string(paint_status);
            }
            return false;
        }

        cairo_surface_flush(scaled_surface.get());
        const int texture_stride = cairo_image_surface_get_stride(
            scaled_surface.get()
        );
        const gsize texture_bytes = static_cast<gsize>(texture_stride) *
            static_cast<gsize>(cache.height);
        gpointer texture_data = g_memdup2(
            cairo_image_surface_get_data(scaled_surface.get()),
            texture_bytes
        );
        GBytes* bytes = g_bytes_new_take(texture_data, texture_bytes);
        cache.texture.reset(gdk_memory_texture_new(
            cache.width,
            cache.height,
            kCairoArgb32MemoryFormat,
            bytes,
            static_cast<gsize>(texture_stride)
        ));
        g_bytes_unref(bytes);
        if (cache.texture == nullptr) {
            if (error_message != nullptr) {
                *error_message = "Unable to create GSK hair texture for " +
                    layer.id;
            }
            return false;
        }

        cache.bands.reserve(mesh->bands().size());
        for (const auto& band : mesh->bands()) {
            const double scaled_height =
                static_cast<double>(band.height) * render_scale;
            if (scaled_height <= 0.01) continue;

            cache.bands.push_back({
                .y = static_cast<double>(band.y) * render_scale,
                .height = scaled_height,
                .movement_weight = std::pow(
                    std::clamp(band.weight, 0.0, 1.0),
                    1.12
                ),
                .outer_x = band.outer_x * render_scale,
                .inner_x = band.inner_x * render_scale,
            });
        }
        if (cache.bands.empty()) {
            if (error_message != nullptr) {
                *error_message = "Scaled hair cache has no drawable bands for " +
                    layer.id;
            }
            return false;
        }
        if (!build_hair_node_cache(
                cache,
                cache.texture.get(),
                cache.quantized_nodes,
                layer.id,
                error_message
            )) {
            return false;
        }

        hair_render_caches_.emplace(layer.id, std::move(cache));
        cached_source_assets.insert(layer.asset_id);
    }

    // Hair is rendered exclusively from immutable final-size GDK textures.
    // Releasing the original decoded PNG surfaces avoids retaining duplicate
    // CPU-side copies after the GSK renderer has its texture source.
    for (const auto& asset_id : cached_source_assets) surfaces_.erase(asset_id);
    return true;
}

bool CharacterCompositor::ensure_flow_caches(std::string* error_message) {
    if (flow_caches_loaded_) return true;

    for (const CharacterLayer& layer : manifest_.layers) {
        if (!layer.visible ||
            layer.renderer != CharacterLayerRenderer::HairMesh ||
            layer.flow_asset_id.empty() || layer.flow_strength <= 0.0) {
            continue;
        }

        const auto cache = hair_render_caches_.find(layer.id);
        if (cache == hair_render_caches_.end()) {
            if (error_message != nullptr) {
                *error_message = "Missing mesh cache for flow layer " + layer.id;
            }
            release_flow_caches();
            return false;
        }
        if (!build_flow_cache_for_layer(layer, cache->second, error_message)) {
            release_flow_caches();
            return false;
        }
    }

    // A rig with no flow-enabled layer may still enter MeshFlow mode; it simply
    // behaves like mesh-only without keeping a useless animation tick alive.
    flow_caches_loaded_ = true;
    return true;
}

bool CharacterCompositor::build_flow_cache_for_layer(
    const CharacterLayer& layer,
    HairRenderCache& cache,
    std::string* error_message
) {
    if (!cache.flow_quantized_nodes.empty()) return true;

    const CharacterAsset* source_asset = manifest_.find_asset(layer.asset_id);
    const CharacterAsset* mask_asset = manifest_.find_asset(layer.mask_asset_id);
    const CharacterAsset* flow_asset = manifest_.find_asset(layer.flow_asset_id);
    if (source_asset == nullptr || mask_asset == nullptr || flow_asset == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Missing source, movement mask, or flow map for " +
                layer.id;
        }
        return false;
    }

    const auto load_scaled_surface = [&](const CharacterAsset& asset,
                                         std::string_view label) -> SurfacePtr {
        SurfacePtr decoded(cairo_image_surface_create_from_png(asset.path.c_str()));
        const cairo_status_t decoded_status = cairo_surface_status(decoded.get());
        if (decoded_status != CAIRO_STATUS_SUCCESS ||
            cairo_image_surface_get_format(decoded.get()) != CAIRO_FORMAT_ARGB32 ||
            cairo_image_surface_get_width(decoded.get()) != asset.size.width ||
            cairo_image_surface_get_height(decoded.get()) != asset.size.height) {
            if (error_message != nullptr) {
                *error_message = "Unable to decode ARGB32 " + std::string(label) +
                    " " + asset.path.string();
            }
            return {};
        }

        SurfacePtr scaled(cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32,
            cache.width,
            cache.height
        ));
        if (cairo_surface_status(scaled.get()) != CAIRO_STATUS_SUCCESS) {
            if (error_message != nullptr) {
                *error_message = "Unable to allocate scaled " +
                    std::string(label) + " for " + layer.id;
            }
            return {};
        }

        cairo_t* cr = cairo_create(scaled.get());
        if (cr == nullptr || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
            if (cr != nullptr) cairo_destroy(cr);
            if (error_message != nullptr) {
                *error_message = "Unable to create scaled " +
                    std::string(label) + " context for " + layer.id;
            }
            return {};
        }

        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_scale(
            cr,
            static_cast<double>(cache.width) /
                static_cast<double>(asset.size.width),
            static_cast<double>(cache.height) /
                static_cast<double>(asset.size.height)
        );
        cairo_set_source_surface(cr, decoded.get(), 0.0, 0.0);
        if (cairo_pattern_t* pattern = cairo_get_source(cr); pattern != nullptr) {
            cairo_pattern_set_filter(pattern, CAIRO_FILTER_BILINEAR);
            cairo_pattern_set_extend(pattern, CAIRO_EXTEND_NONE);
        }
        cairo_paint(cr);
        const cairo_status_t paint_status = cairo_status(cr);
        cairo_destroy(cr);
        if (paint_status != CAIRO_STATUS_SUCCESS) {
            if (error_message != nullptr) {
                *error_message = "Unable to scale " + std::string(label) +
                    " for " + layer.id;
            }
            return {};
        }
        cairo_surface_flush(scaled.get());
        return scaled;
    };

    SurfacePtr scaled_source = load_scaled_surface(*source_asset, "hair texture");
    SurfacePtr scaled_mask = load_scaled_surface(*mask_asset, "movement mask");
    SurfacePtr scaled_flow = load_scaled_surface(*flow_asset, "flow map");
    if (!scaled_source || !scaled_mask || !scaled_flow) return false;

    const Argb32ImageView source_view{
        cairo_image_surface_get_data(scaled_source.get()),
        cache.width,
        cache.height,
        cairo_image_surface_get_stride(scaled_source.get()),
    };
    const Argb32ImageView flow_view{
        cairo_image_surface_get_data(scaled_flow.get()),
        cache.width,
        cache.height,
        cairo_image_surface_get_stride(scaled_flow.get()),
    };
    const Argb32ImageView mask_view{
        cairo_image_surface_get_data(scaled_mask.get()),
        cache.width,
        cache.height,
        cairo_image_surface_get_stride(scaled_mask.get()),
    };

    cache.flow_min_displacement = kFlowPoseMinDisplacement;
    cache.flow_step = kFlowPoseStep;
    const int flow_pose_count = static_cast<int>(std::llround(
        (kFlowPoseMaxDisplacement - kFlowPoseMinDisplacement) /
        kFlowPoseStep
    )) + 1;
    cache.flow_poses.reserve(static_cast<std::size_t>(flow_pose_count));

    for (int pose_index = 0; pose_index < flow_pose_count; ++pose_index) {
        HairFlowPose pose;
        pose.displacement = kFlowPoseMinDisplacement +
            (static_cast<double>(pose_index) * kFlowPoseStep);
        auto warped = warp_hair_argb32(
            source_view,
            flow_view,
            mask_view,
            pose.displacement,
            error_message
        );
        if (!warped) {
            release_flow_cache(cache);
            return false;
        }

        const gsize byte_count = static_cast<gsize>(warped->size());
        gpointer data = g_memdup2(warped->data(), byte_count);
        GBytes* pose_bytes = g_bytes_new_take(data, byte_count);
        pose.texture.reset(gdk_memory_texture_new(
            cache.width,
            cache.height,
            kCairoArgb32MemoryFormat,
            pose_bytes,
            static_cast<gsize>(cache.width * 4)
        ));
        g_bytes_unref(pose_bytes);
        if (pose.texture == nullptr ||
            !build_hair_node_cache(
                cache,
                pose.texture.get(),
                pose.quantized_nodes,
                layer.id + " flow pose",
                error_message
            )) {
            release_flow_cache(cache);
            return false;
        }
        cache.flow_poses.push_back(std::move(pose));
    }

    if (!build_flow_node_cache(cache, layer.id, error_message)) {
        release_flow_cache(cache);
        return false;
    }
    return true;
}

void CharacterCompositor::release_flow_cache(HairRenderCache& cache) noexcept {
    std::vector<RenderNodePtr>().swap(cache.flow_quantized_nodes);
    cache.flow_mesh_pose_count = 0U;
    cache.flow_node_min_displacement = 0.0;
    cache.flow_node_step = 1.0;

    std::vector<HairFlowPose>().swap(cache.flow_poses);
    cache.flow_min_displacement = 0.0;
    cache.flow_step = 1.0;
}

void CharacterCompositor::release_flow_caches() noexcept {
    for (auto& [layer_id, cache] : hair_render_caches_) {
        static_cast<void>(layer_id);
        release_flow_cache(cache);
    }
    flow_caches_loaded_ = false;
    flow_elapsed_seconds_ = 0.0;

#if defined(__GLIBC__)
    // Flow mode owns several megabytes of warped textures and immutable GSK
    // node metadata. Returning to mesh is an explicit low-memory action, so
    // ask glibc to release the now-empty flow-cache pages promptly.
    static_cast<void>(malloc_trim(0));
#endif
}

void CharacterCompositor::paint_surface(
    cairo_t* cr,
    cairo_surface_t* surface
) {
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    if (cairo_pattern_t* source = cairo_get_source(cr); source != nullptr) {
        cairo_pattern_set_filter(source, CAIRO_FILTER_BILINEAR);
        cairo_pattern_set_extend(source, CAIRO_EXTEND_NONE);
    }
    cairo_paint(cr);
}

void CharacterCompositor::append_hair_mesh_geometry(
    GtkSnapshot* snapshot,
    const HairRenderCache& cache,
    GdkTexture* texture,
    double origin_x,
    double origin_y,
    double tip_offset_logical_pixels
) {
    if (snapshot == nullptr || texture == nullptr) return;

    graphene_rect_t texture_bounds;
    graphene_rect_init(
        &texture_bounds,
        static_cast<float>(origin_x),
        static_cast<float>(origin_y),
        static_cast<float>(cache.width),
        static_cast<float>(cache.height)
    );
    if (std::abs(tip_offset_logical_pixels) <= 0.01) {
        gtk_snapshot_append_scaled_texture(
            snapshot,
            texture,
            GSK_SCALING_FILTER_LINEAR,
            &texture_bounds
        );
        return;
    }

    constexpr double clip_bleed = 2.0;
    graphene_rect_t local_texture_bounds;
    graphene_rect_init(
        &local_texture_bounds,
        0.0F,
        0.0F,
        static_cast<float>(cache.width),
        static_cast<float>(cache.height)
    );

    // Adjacent clip rectangles can expose a hairline crack when their shared
    // edge lands between device pixels. Give each upper strip one logical
    // pixel of downward coverage; the following strip is drawn afterward and
    // remains authoritative in the overlap. This removes sampling seams
    // without changing the deformation pivot or visible silhouette.
    constexpr double vertical_overlap = 1.0;
    for (std::size_t band_index = 0; band_index < cache.bands.size(); ++band_index) {
        const auto& band = cache.bands[band_index];
        const double outer_displacement =
            tip_offset_logical_pixels * band.movement_weight;
        const double visible_span = std::max(
            band.inner_x - band.outer_x,
            1.0
        );
        const double horizontal_scale = 1.0 -
            (outer_displacement / visible_span);
        const double horizontal_translation =
            band.outer_x + outer_displacement -
            (horizontal_scale * band.outer_x);

        const double displaced_outer = band.outer_x + outer_displacement;
        const double clip_left = std::min(band.outer_x, displaced_outer) -
            clip_bleed;
        const double clip_right = std::max(band.inner_x, displaced_outer) +
            clip_bleed;

        const double clip_height = band.height +
            (band_index + 1U < cache.bands.size() ? vertical_overlap : 0.0);
        graphene_rect_t clip_bounds;
        graphene_rect_init(
            &clip_bounds,
            static_cast<float>(origin_x + clip_left),
            static_cast<float>(origin_y + band.y),
            static_cast<float>(std::max(clip_right - clip_left, 1.0)),
            static_cast<float>(clip_height)
        );
        graphene_point_t translation;
        graphene_point_init(
            &translation,
            static_cast<float>(origin_x + horizontal_translation),
            static_cast<float>(origin_y)
        );

        gtk_snapshot_save(snapshot);
        gtk_snapshot_push_clip(snapshot, &clip_bounds);
        gtk_snapshot_translate(snapshot, &translation);
        gtk_snapshot_scale(
            snapshot,
            static_cast<float>(horizontal_scale),
            1.0F
        );
        gtk_snapshot_append_scaled_texture(
            snapshot,
            texture,
            GSK_SCALING_FILTER_LINEAR,
            &local_texture_bounds
        );
        gtk_snapshot_pop(snapshot);
        gtk_snapshot_restore(snapshot);
    }
}

bool CharacterCompositor::build_hair_node_cache(
    HairRenderCache& cache,
    GdkTexture* texture,
    std::vector<RenderNodePtr>& output,
    const std::string& layer_id,
    std::string* error_message
) {
    cache.node_min_offset = kHairNodeMinOffset;
    cache.node_step = kHairNodeStep;
    const int node_count = static_cast<int>(std::llround(
        (kHairNodeMaxOffset - kHairNodeMinOffset) / kHairNodeStep
    )) + 1;
    if (node_count <= 0) {
        if (error_message != nullptr) {
            *error_message = "Invalid quantized hair node range for " + layer_id;
        }
        return false;
    }

    if (texture == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Missing texture while building hair nodes for " + layer_id;
        }
        return false;
    }
    output.reserve(static_cast<std::size_t>(node_count));
    for (int index = 0; index < node_count; ++index) {
        const double offset = cache.node_min_offset +
            (static_cast<double>(index) * cache.node_step);
        GtkSnapshot* snapshot = gtk_snapshot_new();
        if (snapshot == nullptr) {
            if (error_message != nullptr) {
                *error_message = "Unable to allocate hair snapshot for " + layer_id;
            }
            return false;
        }

        append_hair_mesh_geometry(snapshot, cache, texture, 0.0, 0.0, offset);
        RenderNodePtr node(gtk_snapshot_free_to_node(snapshot));
        if (node == nullptr) {
            if (error_message != nullptr) {
                *error_message = "Unable to prebuild hair render node for " +
                    layer_id;
            }
            return false;
        }
        output.push_back(std::move(node));
    }
    return true;
}

std::size_t CharacterCompositor::quantized_hair_node_index(
    const HairRenderCache& cache,
    double tip_offset_logical_pixels
) noexcept {
    if (cache.quantized_nodes.empty() || cache.node_step <= 0.0) return 0;

    const long requested_index = std::lround(
        (tip_offset_logical_pixels - cache.node_min_offset) / cache.node_step
    );
    const long maximum_index = static_cast<long>(
        cache.quantized_nodes.size() - 1U
    );
    return static_cast<std::size_t>(
        std::clamp(requested_index, 0L, maximum_index)
    );
}

void CharacterCompositor::append_cached_hair_mesh(
    GtkSnapshot* snapshot,
    const HairRenderCache& cache,
    double origin_x,
    double origin_y,
    double tip_offset_logical_pixels
) {
    if (snapshot == nullptr || cache.quantized_nodes.empty() ||
        cache.node_step <= 0.0) {
        return;
    }

    GskRenderNode* node = cache.quantized_nodes[
        quantized_hair_node_index(cache, tip_offset_logical_pixels)
    ].get();
    if (node == nullptr) return;

    graphene_point_t translation;
    graphene_point_init(
        &translation,
        static_cast<float>(origin_x),
        static_cast<float>(origin_y)
    );
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &translation);
    gtk_snapshot_append_node(snapshot, node);
    gtk_snapshot_restore(snapshot);
}

bool CharacterCompositor::build_flow_node_cache(
    HairRenderCache& cache,
    const std::string& layer_id,
    std::string* error_message
) {
    if (cache.flow_poses.empty() || cache.quantized_nodes.empty() ||
        cache.flow_step <= 0.0 || kFlowNodeStep <= 0.0) {
        return true;
    }

    cache.flow_node_min_displacement = kFlowPoseMinDisplacement;
    cache.flow_node_step = kFlowNodeStep;
    cache.flow_mesh_pose_count = cache.quantized_nodes.size();
    const int flow_node_count = static_cast<int>(std::llround(
        (kFlowPoseMaxDisplacement - kFlowPoseMinDisplacement) /
        kFlowNodeStep
    )) + 1;
    if (flow_node_count <= 0) {
        if (error_message != nullptr) {
            *error_message = "Invalid quantized flow-node range for " + layer_id;
        }
        return false;
    }

    const std::size_t total_nodes =
        static_cast<std::size_t>(flow_node_count) *
        cache.flow_mesh_pose_count;
    cache.flow_quantized_nodes.reserve(total_nodes);

    for (int flow_index = 0; flow_index < flow_node_count; ++flow_index) {
        const double displacement = cache.flow_node_min_displacement +
            (static_cast<double>(flow_index) * cache.flow_node_step);
        const double raw_pose_index = std::clamp(
            (displacement - cache.flow_min_displacement) / cache.flow_step,
            0.0,
            static_cast<double>(cache.flow_poses.size() - 1U)
        );
        const std::size_t lower_index = static_cast<std::size_t>(
            std::floor(raw_pose_index)
        );
        const std::size_t upper_index = std::min(
            lower_index + 1U,
            cache.flow_poses.size() - 1U
        );
        const float blend = static_cast<float>(
            raw_pose_index - static_cast<double>(lower_index)
        );

        const auto& lower_nodes =
            cache.flow_poses[lower_index].quantized_nodes;
        const auto& upper_nodes =
            cache.flow_poses[upper_index].quantized_nodes;
        if (lower_nodes.size() != cache.flow_mesh_pose_count ||
            upper_nodes.size() != cache.flow_mesh_pose_count) {
            if (error_message != nullptr) {
                *error_message = "Flow and mesh pose counts disagree for " +
                    layer_id;
            }
            return false;
        }

        for (std::size_t mesh_index = 0;
             mesh_index < cache.flow_mesh_pose_count;
             ++mesh_index) {
            GskRenderNode* lower = lower_nodes[mesh_index].get();
            GskRenderNode* upper = upper_nodes[mesh_index].get();
            if (lower == nullptr || upper == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Missing flow child node for " + layer_id;
                }
                return false;
            }

            GskRenderNode* node = nullptr;
            if (lower_index == upper_index || blend <= 0.0001F) {
                node = gsk_render_node_ref(lower);
            } else if (blend >= 0.9999F) {
                node = gsk_render_node_ref(upper);
            } else {
                node = gsk_cross_fade_node_new(lower, upper, blend);
            }
            if (node == nullptr) {
                if (error_message != nullptr) {
                    *error_message = "Unable to prebuild flow node for " +
                        layer_id;
                }
                return false;
            }
            cache.flow_quantized_nodes.emplace_back(node);
        }
    }
    return true;
}

std::size_t CharacterCompositor::quantized_flow_node_index(
    const HairRenderCache& cache,
    double flow_displacement
) noexcept {
    if (cache.flow_quantized_nodes.empty() ||
        cache.flow_mesh_pose_count == 0U || cache.flow_node_step <= 0.0) {
        return 0U;
    }

    const std::size_t flow_pose_count =
        cache.flow_quantized_nodes.size() / cache.flow_mesh_pose_count;
    if (flow_pose_count == 0U) return 0U;

    const long requested_index = std::lround(
        (flow_displacement - cache.flow_node_min_displacement) /
        cache.flow_node_step
    );
    const long maximum_index = static_cast<long>(flow_pose_count - 1U);
    return static_cast<std::size_t>(
        std::clamp(requested_index, 0L, maximum_index)
    );
}

void CharacterCompositor::append_cached_hair_mesh_flow(
    GtkSnapshot* snapshot,
    const HairRenderCache& cache,
    double origin_x,
    double origin_y,
    double tip_offset_logical_pixels,
    double flow_displacement
) {
    if (snapshot == nullptr || cache.flow_quantized_nodes.empty() ||
        cache.flow_mesh_pose_count == 0U || cache.flow_node_step <= 0.0) {
        append_cached_hair_mesh(
            snapshot,
            cache,
            origin_x,
            origin_y,
            tip_offset_logical_pixels
        );
        return;
    }

    const std::size_t mesh_index = quantized_hair_node_index(
        cache,
        tip_offset_logical_pixels
    );
    if (mesh_index >= cache.flow_mesh_pose_count) return;

    const std::size_t flow_index = quantized_flow_node_index(
        cache,
        flow_displacement
    );
    const std::size_t flattened_index =
        (flow_index * cache.flow_mesh_pose_count) + mesh_index;
    if (flattened_index >= cache.flow_quantized_nodes.size()) return;

    GskRenderNode* node = cache.flow_quantized_nodes[flattened_index].get();
    if (node == nullptr) return;

    graphene_point_t translation;
    graphene_point_init(
        &translation,
        static_cast<float>(origin_x),
        static_cast<float>(origin_y)
    );
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &translation);
    gtk_snapshot_append_node(snapshot, node);
    gtk_snapshot_restore(snapshot);
}

void CharacterCompositor::draw_callback(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer raw
) {
    const auto& group = *static_cast<DrawGroup*>(raw);
    if (group.owner != nullptr) {
        group.owner->draw_group(group, cr, width, height);
    }
}

void CharacterCompositor::snapshot_callback(
    GtkWidget* widget,
    GtkSnapshot* snapshot,
    gpointer raw
) {
    const auto& group = *static_cast<DrawGroup*>(raw);
    if (group.owner != nullptr) {
        group.owner->snapshot_hair_group(
            group,
            snapshot,
            gtk_widget_get_width(widget),
            gtk_widget_get_height(widget)
        );
    }
}

void CharacterCompositor::draw_group(
    const DrawGroup& group,
    cairo_t* cr,
    int width,
    int height
) const {
    if (width <= 0 || height <= 0 || manifest_.source_canvas.height <= 0) return;

    const auto& animation = animator_.sample();
    if (animation.visibility <= 0.0001) return;

    const double target_height =
        static_cast<double>(host_geometry_.surface_height) *
        manifest_.placement.height_fraction;
    const double scale = target_height /
        static_cast<double>(manifest_.source_canvas.height);
    const double source_anchor_x = manifest_.placement.source_anchor.x *
        static_cast<double>(manifest_.source_canvas.width);
    const double source_anchor_y = manifest_.placement.source_anchor.y *
        static_cast<double>(manifest_.source_canvas.height);
    const double origin_x = host_geometry_.occlusion_left +
        manifest_.placement.host_offset.x - (source_anchor_x * scale);
    const double origin_y = host_geometry_.occlusion_top +
        manifest_.placement.host_offset.y - (source_anchor_y * scale);

    for (const std::size_t layer_index : group.layer_indices) {
        if (layer_index >= manifest_.layers.size()) continue;
        const CharacterLayer& layer = manifest_.layers[layer_index];
        if (!layer.visible) continue;

        const std::string* asset_id = selected_asset_id(layer);
        if (asset_id == nullptr) continue;
        const auto* asset = manifest_.find_asset(*asset_id);
        if (asset == nullptr) continue;
        if (layer.renderer == CharacterLayerRenderer::HairMesh) continue;
        const auto surface = surfaces_.find(*asset_id);
        if (surface == surfaces_.end()) continue;

        double x = 0.0;
        double y = 0.0;
        if (layer.placement == CharacterLayerPlacement::SourceCanvas) {
            x = origin_x + (asset->offset.x * scale);
            y = origin_y + (asset->offset.y * scale);
        } else {
            x = host_geometry_.occlusion_left + layer.host_offset.x;
            y = host_geometry_.occlusion_top + layer.host_offset.y;
        }
        x -= group.host_x;
        y -= group.host_y;

        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, scale, scale);
        paint_surface(cr, surface->second.get());
        cairo_restore(cr);

        if (debug_enabled()) {
            cairo_save(cr);
            cairo_set_source_rgba(
                cr,
                group.plane == CharacterPlane::Back ? 0.25 : 1.0,
                group.plane == CharacterPlane::Back ? 0.85 : 0.55,
                0.95,
                0.7
            );
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(
                cr,
                x,
                y,
                static_cast<double>(asset->size.width) * scale,
                static_cast<double>(asset->size.height) * scale
            );
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    if (debug_enabled() && group.draw_debug_anchor) {
        const double anchor_x = host_geometry_.occlusion_left +
            manifest_.placement.host_offset.x - group.host_x;
        const double anchor_y = host_geometry_.occlusion_top +
            manifest_.placement.host_offset.y - group.host_y;
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1.0, 0.2, 0.2, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, anchor_x - 7.0, anchor_y);
        cairo_line_to(cr, anchor_x + 7.0, anchor_y);
        cairo_move_to(cr, anchor_x, anchor_y - 7.0);
        cairo_line_to(cr, anchor_x, anchor_y + 7.0);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

void CharacterCompositor::snapshot_hair_group(
    const DrawGroup& group,
    GtkSnapshot* snapshot,
    int width,
    int height
) const {
    if (snapshot == nullptr || width <= 0 || height <= 0 ||
        manifest_.source_canvas.height <= 0) {
        return;
    }

    const auto& animation = animator_.sample();
    if (animation.visibility <= 0.0001) return;

    const double target_height =
        static_cast<double>(host_geometry_.surface_height) *
        manifest_.placement.height_fraction;
    const double scale = target_height /
        static_cast<double>(manifest_.source_canvas.height);
    const double source_anchor_x = manifest_.placement.source_anchor.x *
        static_cast<double>(manifest_.source_canvas.width);
    const double source_anchor_y = manifest_.placement.source_anchor.y *
        static_cast<double>(manifest_.source_canvas.height);
    const double origin_x = host_geometry_.occlusion_left +
        manifest_.placement.host_offset.x - (source_anchor_x * scale);
    const double origin_y = host_geometry_.occlusion_top +
        manifest_.placement.host_offset.y - (source_anchor_y * scale);

    for (const std::size_t layer_index : group.layer_indices) {
        if (layer_index >= manifest_.layers.size()) continue;
        const CharacterLayer& layer = manifest_.layers[layer_index];
        if (!layer.visible ||
            layer.renderer != CharacterLayerRenderer::HairMesh) {
            continue;
        }

        const CharacterAsset* asset = manifest_.find_asset(layer.asset_id);
        const auto cache = hair_render_caches_.find(layer.id);
        if (asset == nullptr || cache == hair_render_caches_.end()) continue;

        double x = 0.0;
        double y = 0.0;
        if (layer.placement == CharacterLayerPlacement::SourceCanvas) {
            x = origin_x + (asset->offset.x * scale);
            y = origin_y + (asset->offset.y * scale);
        } else {
            x = host_geometry_.occlusion_left + layer.host_offset.x;
            y = host_geometry_.occlusion_top + layer.host_offset.y;
        }
        x -= group.host_x;
        y -= group.host_y;

        const double tip_offset_logical_pixels =
            hair_mode_ == CharacterHairMode::Static
                ? 0.0
                : (animation.hair_tip_offset_x * layer.mesh_strength) +
                    idle_hair_offset(layer);
        if (hair_mode_ == CharacterHairMode::MeshFlow &&
            !cache->second.flow_poses.empty()) {
            append_cached_hair_mesh_flow(
                snapshot,
                cache->second,
                x,
                y,
                tip_offset_logical_pixels,
                flow_hair_displacement(layer)
            );
        } else {
            append_cached_hair_mesh(
                snapshot,
                cache->second,
                x,
                y,
                tip_offset_logical_pixels
            );
        }
    }
}

void CharacterCompositor::queue_draw() {
    for (const auto& group : draw_groups_) {
        if (group->widget == nullptr) continue;
        if (group->kind == DrawGroupKind::Hair) {
            group->last_hair_pose_signature = hair_pose_signature(*group);
            group->has_hair_pose_signature = true;
        }
        gtk_widget_queue_draw(group->widget);
    }
}

} // namespace realmheart::animation::character
