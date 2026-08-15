#pragma once

#include "relictombs/RelictombsProtocol.hpp"
#include "relictombs/RelictombsSelection.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

typedef struct _GtkApplication GtkApplication;

namespace realmheart::relictombs {

// Phase 1 static skeleton: a fullscreen arch with the selected wallpaper
// rendered behind (and visible through) the transparent portal hole. There is
// no thumbnail rail, no fragment sprites, no apply-state animation yet — the
// arch itself is the only preview (guide rule: the arch is the preview).
//
// Lifecycle: prepare() builds the hidden window and resolves the tiered base
// asset. preload() warms the selected wallpaper decode so an open lands
// instantly. show() presents the surface and starts browsing. Up/Down cycle
// the selection with an asynchronous decode that swaps only when ready. Enter
// drives the apply handshake (Apply -> backend_prepared -> Commit ->
// backend_committed -> Complete), Esc/Close emits Cancel.
class RelictombsOverlay {
public:
    // Intentionally public: the renderer .cpp needs to name the nested type in
    // GTK callback casts. The definition stays in the .cpp, so no
    // implementation detail leaks through the header.
    struct Impl;

    using ResultCallback = std::function<void(RelictombsResult)>;

    explicit RelictombsOverlay(GtkApplication* application, ResultCallback callback);
    ~RelictombsOverlay();

    RelictombsOverlay(const RelictombsOverlay&) = delete;
    RelictombsOverlay& operator=(const RelictombsOverlay&) = delete;

    // Builds the hidden overlay window and resolves the tiered base image.
    // Safe to call exactly once. Returns false with a message on failure.
    [[nodiscard]] bool prepare(std::string* error);

    // Warms the selected wallpaper decode without presenting the surface.
    // The arch portal is the only preview, so this replaces the old
    // thumbnail-rail prewarm entirely.
    [[nodiscard]] bool preload(const RelictombsSelection& selection, std::string* error);

    // Presents the surface and enters browsing. The selection is owned by the
    // overlay from this point; its selected wallpaper is decoded asynchronously.
    [[nodiscard]] bool show(RelictombsSelection selection, std::string* error);

    // Esc / Close: emits Cancel only while browsing; the apply handshake owns
    // its own lifecycle once Enter has been accepted. Idempotent.
    void cancel();

    // Backend apply handshake (driven by the shell's ApplyPrepared command).
    void backend_prepared();

    // Backend reveal finished (ApplyCommitted): emits Complete and hides.
    void backend_committed();

    // Backend apply failed (ApplyFailed): emits Error and hides.
    void backend_failed(std::string_view diagnostic);

    [[nodiscard]] bool active() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace realmheart::relictombs