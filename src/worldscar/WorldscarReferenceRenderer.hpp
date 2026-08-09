#pragma once

#include "worldscar/WorldscarSelection.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace realmheart::worldscar {

class WorldscarReferenceRenderer {
public:
    WorldscarReferenceRenderer();
    ~WorldscarReferenceRenderer();

    WorldscarReferenceRenderer(const WorldscarReferenceRenderer&) = delete;
    WorldscarReferenceRenderer& operator=(const WorldscarReferenceRenderer&) = delete;

    [[nodiscard]] GtkWidget* widget() const noexcept;

    // Loads/validates shader source while the helper is warming. This does not
    // require a realized GL context and therefore does not map the overlay.
    [[nodiscard]] bool prepare(std::string* error = nullptr);

    // Warm all visible preview thumbnails while the shell is moving the current
    // workspace away. They decode concurrently at bounded preview resolution;
    // the shader reveals them previous -> selected -> next along the diagonal.
    [[nodiscard]] bool preload_preview(
        const WorldscarPreviewSet& preview,
        std::string* error = nullptr
    );

    // Opportunistically populate the persistent raw thumbnail cache while the
    // helper is idle. Jobs are serial and automatically pause during an active
    // Worldscar session.
    void prewarm_thumbnail_cache(
        const std::vector<std::filesystem::path>& paths
    ) noexcept;

    [[nodiscard]] bool begin(
        const WorldscarPreviewSet& preview,
        std::string* error = nullptr
    );
    void end_session() noexcept;
    void invalidate_candidate_cache() noexcept;
    void poll_async() noexcept;

    // Starts a selection redistribution. The incoming selected texture and the
    // incoming edge chamber are already resident in the six-slot ring; only the
    // newly exposed hidden far neighbour decodes while the transition runs.
    [[nodiscard]] bool begin_navigation(
        const WorldscarPreviewSet& future_preview,
        int* visual_direction = nullptr,
        std::string* error = nullptr
    );
    void set_navigation_progress(double progress) noexcept;
    void complete_navigation(const WorldscarPreviewSet& preview) noexcept;

    void set_open_progress(double progress) noexcept;
    void set_commit_progress(double progress) noexcept;
    void set_finish_progress(double progress) noexcept;
    void reveal() noexcept;

    [[nodiscard]] bool frame_ready() const noexcept;
    [[nodiscard]] bool candidate_ready() const noexcept;
    [[nodiscard]] bool preview_ready() const noexcept;
    [[nodiscard]] bool can_navigate_to(
        const std::filesystem::path& path
    ) const noexcept;
    [[nodiscard]] bool preview_available_for_navigation(
        const WorldscarPreviewSet& preview
    ) const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::string failure_message() const;
    [[nodiscard]] std::uint64_t rendered_frames() const noexcept;

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::worldscar
