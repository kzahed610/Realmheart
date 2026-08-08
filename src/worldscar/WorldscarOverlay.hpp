#pragma once

#include "worldscar/WorldscarProtocol.hpp"
#include "worldscar/WorldscarSelection.hpp"

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>

namespace realmheart::worldscar {

class WorldscarReferenceRenderer;

class WorldscarOverlay {
public:
    using CompletedCallback = std::function<void(WorldscarResult)>;

    WorldscarOverlay(GtkApplication* application, CompletedCallback completed);
    ~WorldscarOverlay();

    WorldscarOverlay(const WorldscarOverlay&) = delete;
    WorldscarOverlay& operator=(const WorldscarOverlay&) = delete;

    [[nodiscard]] bool prepare(std::string* error = nullptr);
    [[nodiscard]] bool preload_preview(
        const WorldscarSelection& selection,
        std::string* error = nullptr
    );
    void prewarm_thumbnail_cache(
        const std::vector<std::filesystem::path>& paths
    ) noexcept;
    [[nodiscard]] bool show(
        WorldscarSelection selection,
        std::string* error = nullptr
    );
    void cancel() noexcept;

    // Apply is transactional: APPLY starts hidden full-resolution preparation,
    // COMMIT waits at the residual slash, and COMPLETE is emitted only after the
    // real wallpaper renderer has revealed the prepared wallpaper and the slash
    // itself has faded to an exact transparent endpoint.
    void backend_prepared() noexcept;
    void backend_committed() noexcept;
    void backend_failed(std::string diagnostic) noexcept;
    void invalidate_candidate_cache() noexcept;

    [[nodiscard]] bool active() const noexcept;

private:
    enum class Phase {
        Idle,
        WaitingFirstFrame,
        Opening,
        Browsing,
        Navigating,
        Cancelling,
        Applying,
        WaitingCancelEndpoint,
        WaitingApplyEndpoint,
        AwaitingPrepared,
        AwaitingBackend,
        Finishing,
        WaitingFinishEndpoint,
        WaitingFailureEndpoint,
    };

    static gboolean tick_callback(
        GtkWidget* widget,
        GdkFrameClock* frame_clock,
        gpointer data
    );
    static gboolean key_pressed_callback(
        GtkEventControllerKey* controller,
        guint keyval,
        guint keycode,
        GdkModifierType state,
        gpointer data
    );

    gboolean tick(GdkFrameClock* frame_clock) noexcept;
    void request_navigation(int direction) noexcept;
    [[nodiscard]] bool start_navigation(int direction) noexcept;
    void request_apply() noexcept;
    void request_backend_commit() noexcept;
    void finish_session(WorldscarResult result, bool notify) noexcept;
    void ensure_tick() noexcept;
    void stop_tick() noexcept;
    void install_transparency_css();
    void remove_transparency_css() noexcept;

    GtkWindow* window_ = nullptr;
    GtkCssProvider* transparency_provider_ = nullptr;
    GtkEventController* key_controller_ = nullptr;
    std::unique_ptr<WorldscarReferenceRenderer> renderer_;
    CompletedCallback completed_;

    std::optional<WorldscarSelection> selection_;
    std::optional<WorldscarSelection> pending_selection_;
    std::string backend_failure_diagnostic_;
    Phase phase_ = Phase::Idle;
    guint tick_id_ = 0;
    gint64 phase_started_us_ = 0;
    double open_progress_ = 0.0;
    double cancel_start_open_ = 0.0;
    std::uint64_t endpoint_frame_target_ = 0;
    int pending_navigation_steps_ = 0;
    bool backend_prepared_ = false;
};

} // namespace realmheart::worldscar
