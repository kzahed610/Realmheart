#pragma once

#include "wallpaper-native/gpu/RenderFrame.hpp"
#include "wallpaper-native/source/RuntimeWallpaperSource.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace realmheart::wallpaper_native {

// A static image wallpaper, from a local file path or from owned bytes.
//
// Owns everything the renderer used to keep for a static wallpaper: source
// retention (needed for a topology re-decode), decode sizing, GdkPixbuf
// decoding, and GL texture lifetime. The renderer only asks for frames.
//
// All work happens on the renderer's GL thread with a current context. That
// includes destruction: releasing a source deletes its GL texture, so the
// renderer must keep the pbuffer current until every reference is gone.
class StaticImageSource : public RuntimeWallpaperSource {
public:
    // Exactly one of path/bytes is meaningful; a source is never both.
    StaticImageSource(std::string path, std::shared_ptr<const std::string> bytes);
    ~StaticImageSource() override;

    StaticImageSource(const StaticImageSource&) = delete;
    StaticImageSource& operator=(const StaticImageSource&) = delete;

    [[nodiscard]] WallpaperMediaKind kind() const noexcept override {
        return WallpaperMediaKind::StaticImage;
    }

    [[nodiscard]] bool prepare(std::string* error_message) override;
    void start(std::chrono::steady_clock::time_point now) noexcept override;
    void stop() noexcept override;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_deadline() const noexcept override;
    SourceTickResult tick(std::chrono::steady_clock::time_point now) noexcept override;
    [[nodiscard]] RenderFrameView current_frame() const noexcept override;

    // A prepared source can only be committed while it holds a frame.
    [[nodiscard]] bool has_frame() const noexcept override {
        return frame_.texture != 0;
    }

    void set_output_requirement(int width, int height) noexcept override;
    [[nodiscard]] bool requires_topology_rederive(
        int required_width,
        int required_height
    ) const noexcept override;
    [[nodiscard]] bool adapt_to_topology(
        int required_width,
        int required_height,
        std::string* error_message
    ) override;

private:
    [[nodiscard]] bool decode_from_file(std::string* error_message);
    [[nodiscard]] bool decode_from_bytes(std::string* error_message);
    [[nodiscard]] bool upload_pixels(
        GdkPixbuf* pixbuf,
        const char* layout_error,
        std::string* error_message
    );
    void release_frame() noexcept;

    std::string path_;
    std::shared_ptr<const std::string> bytes_;
    RenderFrame frame_{};
    int required_width_ = 0;
    int required_height_ = 0;
};

[[nodiscard]] RuntimeSourcePtr make_static_image_source(std::string path);
[[nodiscard]] RuntimeSourcePtr make_static_image_source(
    std::shared_ptr<const std::string> bytes
);

} // namespace realmheart::wallpaper_native
