#pragma once

#include "wallpaper-native/gpu/RenderFrame.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace realmheart::wallpaper_native {

enum class WallpaperMediaKind {
    StaticImage,
    Video,
};

// Outcome of advancing one source by a single tick.
struct SourceTickResult {
    // The source published newer state that has to reach the screen.
    bool redraw_required = false;
    // The source can no longer produce frames. The renderer keeps the last
    // valid frame on screen and reports the error instead of spinning.
    bool failed = false;
    std::string error;
};

// A wallpaper the renderer can display.
//
// The renderer owns Wayland, EGL surfaces, output routing, transaction slots,
// and the event loop. A source owns how its own pixels are produced: an image
// decodes once, a video decodes ahead and wakes the renderer at PTS deadlines.
// Nothing outside a source may know which kind it is.
class RuntimeWallpaperSource {
public:
    virtual ~RuntimeWallpaperSource() = default;

    [[nodiscard]] virtual WallpaperMediaKind kind() const noexcept = 0;

    // A dynamic source has its own timeline and timed work.
    [[nodiscard]] bool dynamic() const noexcept {
        return kind() == WallpaperMediaKind::Video;
    }

    // Resource validation and the first decode/import happen here. A prepared
    // source must not alter visible output, so it can be discarded without
    // ever having been drawn.
    [[nodiscard]] virtual bool prepare(std::string* error_message) = 0;

    // Called when this source becomes authoritative.
    virtual void start(std::chrono::steady_clock::time_point now) = 0;
    virtual void stop() noexcept = 0;

    // nullopt means the source has no timed work of its own, which is what
    // keeps a stable static wallpaper fully idle.
    [[nodiscard]] virtual std::optional<std::chrono::steady_clock::time_point>
    next_deadline() const noexcept = 0;

    // Advance source state for 'now'; may publish a newer frame.
    virtual SourceTickResult tick(std::chrono::steady_clock::time_point now) = 0;

    [[nodiscard]] virtual RenderFrameView current_frame() const noexcept = 0;

    // True while the source holds a drawable frame.
    [[nodiscard]] virtual bool has_frame() const noexcept = 0;

    // True while a prepared source can be committed. A source can be
    // committable before it has ever drawn a frame: a video's first decoded
    // frame is only presented once playback starts.
    [[nodiscard]] virtual bool committable() const noexcept = 0;

    // Records the output pixel box this source must cover. Static sources use
    // it to decide when a topology change forces a re-decode; video sources
    // keep their encoded geometry and ignore it.
    virtual void set_output_requirement(int width, int height) noexcept = 0;

    // True when covering the given output pixel box would force this source to
    // re-derive its frame. The renderer decides from this whether an output
    // needs an extra draw after a topology change; callers that already drew
    // (layer-surface configure, output scale) rely on it to skip redundant
    // work. Video frames keep their encoded geometry, so this is false.
    [[nodiscard]] virtual bool requires_topology_rederive(
        int required_width,
        int required_height
    ) const noexcept = 0;

    // Re-derives the source for the output pixel box it must cover, without
    // restarting playback. Static sources re-decode when the box grows; video
    // sources only need a redraw because their geometry is encoded geometry.
    [[nodiscard]] virtual bool adapt_to_topology(
        int required_width,
        int required_height,
        std::string* error_message
    ) = 0;

protected:
    RuntimeWallpaperSource() = default;
};

// Shared so one decoded source can feed several outputs (renderer docs
// section 11); an output override and the global wallpaper may be the same
// source, with one decode timeline and one frame queue.
using RuntimeSourcePtr = std::shared_ptr<RuntimeWallpaperSource>;

} // namespace realmheart::wallpaper_native
