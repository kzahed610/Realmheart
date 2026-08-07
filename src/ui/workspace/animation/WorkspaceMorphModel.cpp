#include "ui/workspace/animation/WorkspaceMorphModel.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::workspace::animation {
namespace {

constexpr double kEndpointEpsilon = 0.0005;
constexpr double kFallbackSourceWidth = 25.0;
constexpr double kFallbackSourceHeight = 31.0;
constexpr double kFallbackSourceX = 15.5;
constexpr double kFallbackSourceSpacing = 38.0;

[[nodiscard]] double clamp_unit(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double smooth(double value) noexcept {
    const double t = clamp_unit(value);
    return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] double lerp(double from, double to, double progress) noexcept {
    return from + (to - from) * progress;
}

[[nodiscard]] std::size_t style_index_for_workspace(int workspace_id) noexcept {
    const int normalized = std::max(1, workspace_id) - 1;
    return static_cast<std::size_t>(normalized) % kWorkspaceMorphBandCount;
}

[[nodiscard]] WorkspaceMorphRect fallback_source(
    std::size_t index,
    double height
) noexcept {
    const double center_y = height * 0.5 +
        (static_cast<double>(index) - 1.5) * kFallbackSourceSpacing;
    return {
        kFallbackSourceX,
        center_y - kFallbackSourceHeight * 0.5,
        kFallbackSourceWidth,
        kFallbackSourceHeight,
    };
}

[[nodiscard]] const WorkspaceMorphSource* find_source_for_workspace(
    int workspace_id,
    const std::vector<WorkspaceMorphSource>& sources
) noexcept {
    const auto exact = std::find_if(
        sources.begin(),
        sources.end(),
        [workspace_id](const WorkspaceMorphSource& source) {
            return source.workspace_id == workspace_id;
        }
    );
    return exact != sources.end() ? &*exact : nullptr;
}

[[nodiscard]] WorkspaceMorphRect source_for_workspace(
    int workspace_id,
    std::size_t index,
    const std::vector<WorkspaceMorphSource>& sources,
    double height
) noexcept {
    const auto* exact = find_source_for_workspace(workspace_id, sources);
    if (exact != nullptr) return exact->bounds;
    return fallback_source(index, height);
}

} // namespace

double workspace_morph_stage(
    double progress,
    double start,
    double end
) noexcept {
    const double p = clamp_unit(progress);
    if (!std::isfinite(start) || !std::isfinite(end) || end <= start) {
        return p >= end ? 1.0 : 0.0;
    }
    return clamp_unit((p - start) / (end - start));
}

std::vector<WorkspaceMorphSource>
scale_workspace_morph_sources_to_reference(
    const std::vector<WorkspaceMorphSource>& sources,
    double scale_x,
    double scale_y
) noexcept {
    const double safe_scale_x = std::isfinite(scale_x) && scale_x > 0.0
        ? scale_x
        : 1.0;
    const double safe_scale_y = std::isfinite(scale_y) && scale_y > 0.0
        ? scale_y
        : 1.0;

    std::vector<WorkspaceMorphSource> scaled_sources;
    scaled_sources.reserve(sources.size());
    for (const auto& source : sources) {
        auto scaled = source;
        scaled.bounds.x /= safe_scale_x;
        scaled.bounds.y /= safe_scale_y;
        scaled.bounds.width /= safe_scale_x;
        scaled.bounds.height /= safe_scale_y;
        scaled_sources.push_back(scaled);
    }
    return scaled_sources;
}

WorkspaceMorphLayout build_workspace_morph_layout(
    const std::array<int, kWorkspaceMorphBandCount>& workspace_ids,
    const std::array<double, kWorkspaceMorphBandCount>& destination_heights,
    const std::vector<WorkspaceMorphSource>& sources,
    double width,
    double height
) noexcept {
    WorkspaceMorphLayout layout;
    layout.width = std::max(1.0, std::isfinite(width) ? width : 1.0);
    layout.height = std::max(1.0, std::isfinite(height) ? height : 1.0);

    double top = 0.0;
    for (std::size_t index = 0; index < layout.bands.size(); ++index) {
        const double remaining = std::max(0.0, layout.height - top);
        double band_height = std::isfinite(destination_heights[index])
            ? std::max(0.0, destination_heights[index])
            : 0.0;
        if (index + 1 == layout.bands.size()) {
            band_height = remaining;
        } else {
            band_height = std::min(band_height, remaining);
        }

        auto& band = layout.bands[index];
        band.workspace_id = workspace_ids[index];
        band.style_index = style_index_for_workspace(workspace_ids[index]);
        band.source = source_for_workspace(
            workspace_ids[index],
            index,
            sources,
            layout.height
        );
        if (const auto* source = find_source_for_workspace(
                workspace_ids[index],
                sources
            ); source != nullptr) {
            band.active = source->active;
            band.occupied = source->occupied;
        }
        band.destination = {0.0, top, layout.width, band_height};
        top += band_height;
    }
    return layout;
}

double workspace_morph_rune_opacity(double progress) noexcept {
    const double p = clamp_unit(progress);
    // Keep the real rune visible long enough for the elemental seed to visibly
    // originate from it. On close this is traversed in reverse, so the rune
    // returns before the last local glow is absorbed.
    return 1.0 - smooth(workspace_morph_stage(p, 0.085, 0.245));
}

WorkspaceMorphFrame sample_workspace_morph_frame(
    const WorkspaceMorphLayout& layout,
    double progress
) noexcept {
    const double p = clamp_unit(progress);
    WorkspaceMorphFrame frame;
    frame.exact_hidden = p <= kEndpointEpsilon;
    frame.exact_visible = p >= 1.0 - kEndpointEpsilon;

    // Topology, not just timing: every realm begins as a local seed occupying
    // the captured rune bounds. It first breathes a short distance to the
    // right, then unfolds vertically toward its destination band while its
    // left/root edge spreads behind the Aether Spine. Only after that does the
    // main frontier travel across the monitor. Closing samples this exact same
    // function backwards, so the full-height frontier shrinks back into the
    // source rune instead of dying as a vertical line at screen x=0.
    const double seed_bloom = smooth(workspace_morph_stage(p, 0.025, 0.155));
    const double band_unfold = smooth(workspace_morph_stage(p, 0.075, 0.430));
    const double root_spread = smooth(workspace_morph_stage(p, 0.125, 0.390));
    const double propagation = smooth(workspace_morph_stage(p, 0.165, 0.900));
    const double realm_opacity = smooth(workspace_morph_stage(p, 0.035, 0.220));
    const double seed_opacity =
        smooth(workspace_morph_stage(p, 0.018, 0.095)) *
        (1.0 - smooth(workspace_morph_stage(p, 0.240, 0.455)));
    const double identity_opacity = smooth(workspace_morph_stage(p, 0.38, 0.78));
    const double character_opacity = smooth(workspace_morph_stage(p, 0.46, 0.91));
    const double card_opacity = smooth(workspace_morph_stage(p, 0.60, 1.00));
    const double rune_fade = workspace_morph_rune_opacity(p);

    // Let the separators belong to the morph instead of popping in at the
    // terminal handoff. They start only after the realm structure is already
    // readable, then settle before the frontier finishes crossing the output.
    // Closing samples the same curve backwards, so they dissolve naturally
    // rather than disappearing on the first closing frame.
    frame.separator_opacity = smooth(workspace_morph_stage(p, 0.58, 0.92));
    frame.reveal_right = 0.0;

    for (std::size_t index = 0; index < layout.bands.size(); ++index) {
        const auto& band_layout = layout.bands[index];
        auto& band_frame = frame.bands[index];
        const WorkspaceMorphRect source = band_layout.source;
        const WorkspaceMorphRect destination = band_layout.destination;

        band_frame.rune = source;
        band_frame.proxy = source;
        band_frame.proxy_opacity = 0.0;
        band_frame.stroke_opacity = 0.0;
        band_frame.rune_opacity = frame.exact_visible ? 0.0 : rune_fade;

        const double source_left = std::clamp(source.x, 0.0, layout.width);
        const double source_right = std::clamp(
            source.x + source.width,
            source_left,
            layout.width
        );
        const double source_top = std::clamp(source.y, 0.0, layout.height);
        const double source_bottom = std::clamp(
            source.y + source.height,
            source_top,
            layout.height
        );
        const double destination_bottom = std::clamp(
            destination.y + destination.height,
            destination.y,
            layout.height
        );

        const double clip_left = lerp(
            source_left,
            std::clamp(destination.x, 0.0, layout.width),
            root_spread
        );
        const double clip_top = lerp(source_top, destination.y, band_unfold);
        const double clip_bottom = lerp(
            source_bottom,
            destination_bottom,
            band_unfold
        );

        // A short launch makes the birth readable before the main screen-wide
        // propagation starts. It is capped so tiny monitors still remain sane.
        const double launch_distance = std::min(
            92.0,
            std::max(42.0, layout.width * 0.052)
        );
        const double launched_right = std::clamp(
            source_right + launch_distance * seed_bloom,
            source_right,
            layout.width
        );
        const double clip_right = lerp(
            launched_right,
            layout.width,
            propagation
        );

        band_frame.reveal_clip = {
            clip_left,
            clip_top,
            std::max(0.0, clip_right - clip_left),
            std::max(0.0, clip_bottom - clip_top),
        };
        frame.reveal_right = std::max(frame.reveal_right, clip_right);

        band_frame.realm_opacity = frame.exact_visible ? 1.0 : realm_opacity;
        band_frame.seed_opacity = frame.exact_visible ? 0.0 : seed_opacity;
        band_frame.identity_opacity = frame.exact_visible
            ? 1.0
            : identity_opacity;
        band_frame.character_opacity = frame.exact_visible
            ? 1.0
            : character_opacity;
        band_frame.card_opacity = frame.exact_visible ? 1.0 : card_opacity;
    }

    if (frame.exact_visible) {
        frame.separator_opacity = 1.0;
        frame.reveal_right = layout.width;
        for (std::size_t index = 0; index < frame.bands.size(); ++index) {
            frame.bands[index].reveal_clip = layout.bands[index].destination;
            frame.bands[index].seed_opacity = 0.0;
        }
    }
    return frame;
}

} // namespace realmheart::ui::workspace::animation
