#pragma once

#include "animation/character/CharacterAnimator.hpp"
#include "animation/character/CharacterExpressionAnimator.hpp"
#include "animation/character/CharacterHairMode.hpp"
#include "animation/character/CharacterManifest.hpp"
#include "animation/character/HairMesh.hpp"

#include <cairo.h>
#include <cstddef>
#include <filesystem>
#include <gtk/gtk.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realmheart::animation::character {

struct CharacterHostGeometry {
    // The top-left point of the host surface that occludes rear character art.
    double occlusion_left = 0.0;
    double occlusion_top = 0.0;
    int surface_width = 0;
    int surface_height = 0;
};

// Layer compositor plus lifecycle animation, mask-weighted macro hair
// inertia, a restrained low-rate idle sway, discrete blink-hidden facial
// expression swaps, and optional rear-hair directional flow.
class CharacterCompositor {
public:
    static std::unique_ptr<CharacterCompositor> create(
        GtkWidget* back_host,
        GtkWidget* front_host,
        const std::filesystem::path& character_root,
        core::DisplayTier asset_tier,
        core::DisplayTier layout_tier,
        CharacterHostGeometry host_geometry,
        std::string* error_message = nullptr
    );

    ~CharacterCompositor();

    CharacterCompositor(const CharacterCompositor&) = delete;
    CharacterCompositor& operator=(const CharacterCompositor&) = delete;

    void start_enter();
    void start_exit();
    bool set_hair_mode(
        CharacterHairMode mode,
        std::string* error_message = nullptr
    );
    [[nodiscard]] CharacterHairMode hair_mode() const noexcept { return hair_mode_; }
    void queue_draw();
    [[nodiscard]] static constexpr unsigned int exit_duration_ms() noexcept {
        return static_cast<unsigned int>(
            CharacterAnimator::kExitDurationSeconds * 1000.0
        );
    }
    [[nodiscard]] const CharacterManifest& manifest() const { return manifest_; }

private:
    struct SurfaceDeleter {
        void operator()(cairo_surface_t* surface) const;
    };
    struct TextureDeleter {
        void operator()(GdkTexture* texture) const;
    };
    struct RenderNodeDeleter {
        void operator()(GskRenderNode* node) const;
    };
    using SurfacePtr = std::unique_ptr<cairo_surface_t, SurfaceDeleter>;
    using TexturePtr = std::unique_ptr<GdkTexture, TextureDeleter>;
    using RenderNodePtr = std::unique_ptr<GskRenderNode, RenderNodeDeleter>;

    enum class DrawGroupKind {
        Static,
        Hair,
        Expression,
    };

    struct HairRenderBand {
        double y = 0.0;
        double height = 0.0;
        double movement_weight = 0.0;
        double outer_x = 0.0;
        double inner_x = 0.0;
    };

    struct HairFlowPose {
        double displacement = 0.0;
        TexturePtr texture;
        std::vector<RenderNodePtr> quantized_nodes;
    };

    struct HairRenderCache {
        TexturePtr texture;
        int width = 0;
        int height = 0;
        std::vector<HairRenderBand> bands;
        double node_min_offset = 0.0;
        double node_step = 1.0;
        std::vector<RenderNodePtr> quantized_nodes;
        double flow_min_displacement = 0.0;
        double flow_step = 1.0;
        std::vector<HairFlowPose> flow_poses;

        // Cross-faded flow poses are immutable too. Prebuilding a denser
        // displacement ladder means the runtime snapshot appends one cached
        // node instead of constructing and rendering a fresh two-child
        // cross-fade tree on every frame.
        double flow_node_min_displacement = 0.0;
        double flow_node_step = 1.0;
        std::size_t flow_mesh_pose_count = 0;
        std::vector<RenderNodePtr> flow_quantized_nodes;
    };

    // Consecutive layers with the same invalidation behavior share one GTK
    // widget. Static/expression groups remain Cairo drawing areas, while hair
    // groups emit GSK texture nodes so deformation stays off the CPU raster path.
    struct DrawGroup {
        CharacterCompositor* owner = nullptr;
        CharacterPlane plane = CharacterPlane::Back;
        DrawGroupKind kind = DrawGroupKind::Static;
        std::vector<std::size_t> layer_indices;
        GtkWidget* widget = nullptr;
        double host_x = 0.0;
        double host_y = 0.0;
        int width = 0;
        int height = 0;
        bool pin_during_exit = false;
        bool draw_debug_anchor = false;
        std::size_t last_hair_pose_signature = 0;
        bool has_hair_pose_signature = false;
    };

    // A contiguous z-order run whose children share the same lifecycle motion.
    // The wrapper applies translation/opacity in its snapshot, avoiding GTK
    // layout invalidation from moving every individual draw-group widget.
    struct MotionSegment {
        CharacterPlane plane = CharacterPlane::Back;
        bool pin_during_exit = false;
        GtkWidget* widget = nullptr;
        GtkWidget* content = nullptr;
        std::vector<DrawGroup*> groups;
        double host_x = 0.0;
        double host_y = 0.0;
        int width = 0;
        int height = 0;
    };

    CharacterCompositor(
        GtkWidget* back_host,
        GtkWidget* front_host,
        CharacterManifest manifest,
        core::DisplayTier layout_tier,
        CharacterHostGeometry host_geometry
    );

    bool load_surfaces(std::string* error_message);
    bool build_hair_meshes(std::string* error_message);
    bool ensure_flow_caches(std::string* error_message);
    bool build_flow_cache_for_layer(
        const CharacterLayer& layer,
        HairRenderCache& cache,
        std::string* error_message
    );
    static void release_flow_cache(HairRenderCache& cache) noexcept;
    void release_flow_caches() noexcept;
    bool create_draw_groups(std::string* error_message);
    [[nodiscard]] DrawGroupKind classify_layer(
        const CharacterLayer& layer
    ) const noexcept;
    [[nodiscard]] bool is_expression_layer(
        const CharacterLayer& layer
    ) const noexcept;
    [[nodiscard]] GtkWidget* host_for_plane(CharacterPlane plane) const noexcept;
    [[nodiscard]] bool has_group(DrawGroupKind kind) const noexcept;
    static void paint_surface(cairo_t* cr, cairo_surface_t* surface);
    static void append_hair_mesh_geometry(
        GtkSnapshot* snapshot,
        const HairRenderCache& cache,
        GdkTexture* texture,
        double origin_x,
        double origin_y,
        double tip_offset_logical_pixels
    );
    static bool build_hair_node_cache(
        HairRenderCache& cache,
        GdkTexture* texture,
        std::vector<RenderNodePtr>& output,
        const std::string& layer_id,
        std::string* error_message
    );
    static std::size_t quantized_hair_node_index(
        const HairRenderCache& cache,
        double tip_offset_logical_pixels
    ) noexcept;
    static void append_cached_hair_mesh(
        GtkSnapshot* snapshot,
        const HairRenderCache& cache,
        double origin_x,
        double origin_y,
        double tip_offset_logical_pixels
    );
    static bool build_flow_node_cache(
        HairRenderCache& cache,
        const std::string& layer_id,
        std::string* error_message
    );
    static std::size_t quantized_flow_node_index(
        const HairRenderCache& cache,
        double flow_displacement
    ) noexcept;
    static void append_cached_hair_mesh_flow(
        GtkSnapshot* snapshot,
        const HairRenderCache& cache,
        double origin_x,
        double origin_y,
        double tip_offset_logical_pixels,
        double flow_displacement
    );
    static gboolean tick_callback(
        GtkWidget* widget,
        GdkFrameClock* frame_clock,
        gpointer raw
    );
    static gboolean idle_timeout_callback(gpointer raw);
    void apply_motion_state();
    void ensure_tick();
    void stop_tick();
    void ensure_idle_timeout();
    void stop_idle_timeout(bool reset_elapsed);
    gboolean on_tick(GdkFrameClock* frame_clock);
    gboolean on_idle_timeout();
    [[nodiscard]] double idle_hair_offset(
        const CharacterLayer& layer
    ) const noexcept;
    [[nodiscard]] double flow_hair_displacement(
        const CharacterLayer& layer
    ) const noexcept;
    [[nodiscard]] bool flow_tick_active() const noexcept;
    [[nodiscard]] std::size_t hair_pose_signature(
        const DrawGroup& group
    ) const noexcept;
    void queue_hair_draw(bool force = false);
    void queue_lifecycle_draw(gint64 frame_time_us);
    void queue_expression_draw();
    [[nodiscard]] const std::string* selected_asset_id(
        const CharacterLayer& layer
    ) const noexcept;
    static void draw_callback(
        GtkDrawingArea* area,
        cairo_t* cr,
        int width,
        int height,
        gpointer raw
    );
    static void snapshot_callback(
        GtkWidget* widget,
        GtkSnapshot* snapshot,
        gpointer raw
    );
    void draw_group(const DrawGroup& group, cairo_t* cr, int width, int height) const;
    void snapshot_hair_group(
        const DrawGroup& group,
        GtkSnapshot* snapshot,
        int width,
        int height
    ) const;

    GtkWidget* back_host_ = nullptr;
    GtkWidget* front_host_ = nullptr;
    GtkWidget* tick_widget_ = nullptr;
    CharacterManifest manifest_;
    core::DisplayTier layout_tier_ = core::DisplayTier::P1080;
    CharacterAnimator animator_;
    CharacterExpressionAnimator expression_animator_;
    CharacterHostGeometry host_geometry_;
    std::unordered_map<std::string, SurfacePtr> surfaces_;
    std::unordered_map<std::string, HairRenderCache> hair_render_caches_;
    std::vector<std::unique_ptr<DrawGroup>> draw_groups_;
    std::vector<std::unique_ptr<MotionSegment>> motion_segments_;
    guint tick_callback_id_ = 0;
    guint idle_timeout_id_ = 0;
    gint64 last_frame_time_us_ = 0;
    gint64 last_hair_draw_time_us_ = 0;
    gint64 last_idle_time_us_ = 0;
    double idle_elapsed_seconds_ = 0.0;
    double flow_elapsed_seconds_ = 0.0;
    CharacterHairMode hair_mode_ = CharacterHairMode::Mesh;
    bool flow_caches_loaded_ = false;
};

} // namespace realmheart::animation::character
