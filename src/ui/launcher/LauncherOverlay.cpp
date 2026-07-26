#include "ui/launcher/LauncherOverlay.hpp"

#include "core/Command.hpp"
#include "core/TaskExecutor.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/launcher/CommandReceiptOverlay.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <gtk4-layer-shell.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <jpeglib.h>
#include <setjmp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::ui {

struct LauncherOverlay::ClipboardAsyncState {
    std::atomic<LauncherOverlay*> owner{nullptr};
    std::atomic<std::uint64_t> generation{0};
};

struct LauncherOverlay::EmojiAsyncState {
    std::atomic<LauncherOverlay*> owner{nullptr};
    std::atomic<std::uint64_t> generation{0};
};

namespace {

namespace fs = std::filesystem;

constexpr int kSeedApplicationCount = 4;
constexpr int kResultCount = 6;
constexpr std::size_t kClipboardBrowseResultCount = 100;
constexpr std::size_t kClipboardInitialResultCount = 14;
constexpr std::size_t kClipboardResultBatchCount = 14;
constexpr std::size_t kEmojiBrowseResultCount = 2048;
constexpr std::size_t kEmojiInitialResultCount = 18;
constexpr std::size_t kEmojiResultBatchCount = 24;
constexpr int kNormalResultsMaximumHeight = 336;
constexpr int kClipboardResultsMaximumHeight = 540;
constexpr int kEmojiResultsMaximumHeight = 540;
constexpr int kMaximumConstellationApplications = 12;
constexpr int kConstellationNodeWidth = 88;
constexpr int kConstellationNodeHeight = 74;
constexpr double kDragThreshold = 7.0;
constexpr double kConstellationLeftInset = 84.0;
constexpr double kConstellationRightInset = 56.0;
constexpr double kConstellationTopInset = 380.0;
constexpr double kConstellationBottomInset = 58.0;
constexpr double kCentreKeepoutSide = 14.0;
constexpr double kCentreKeepoutBottom = 44.0;
constexpr double kDragLift = 9.0;
constexpr double kDragSpring = 720.0;
constexpr double kDragDamping = 42.0;
constexpr double kSettleSpring = 260.0;
constexpr double kSettleDamping = 14.0;
constexpr double kIdleSpring = 420.0;
constexpr double kIdleDamping = 34.0;
constexpr double kOpacityResponse = 17.0;
constexpr double kVisibilityStagger = 0.085;
constexpr double kVisibilitySpring = 330.0;
constexpr double kVisibilityDamping = 22.0;
constexpr double kSelectionLift = 5.0;
constexpr double kSelectionNodeSpring = 420.0;
constexpr double kSelectionNodeDamping = 28.0;
constexpr double kSelectionIndicatorSpring = 520.0;
constexpr double kSelectionIndicatorDamping = 31.0;
constexpr double kSelectionIndicatorOpacityResponse = 20.0;
constexpr double kSelectionTravelImpulse = 1.35;
constexpr int kSelectionIndicatorWidth = 102;
constexpr int kSelectionIndicatorHeight = 88;
constexpr double kResultSelectionSpring = 620.0;
constexpr double kResultSelectionDamping = 36.0;
constexpr double kResultSelectionHeightSpring = 700.0;
constexpr double kResultSelectionHeightDamping = 40.0;
constexpr double kResultSelectionOpacityResponse = 24.0;
constexpr double kResultSelectionTravelImpulse = 1.18;
constexpr double kResultSelectionStretchFactor = 0.010;
constexpr double kResultSelectionMaximumStretch = 8.0;
constexpr double kResultRowLift = 5.0;
constexpr double kResultRowLiftSpring = 520.0;
constexpr double kResultRowLiftDamping = 25.0;
constexpr double kResultRowLiftImpulse = 74.0;
constexpr double kEmergenceHorizontalCompression = 0.72;
constexpr double kEmergenceEdgeInset = 58.0;
constexpr double kEmergencePeek = 8.0;
constexpr double kEmergenceArc = 18.0;
constexpr double kPositionEpsilon = 0.08;
constexpr double kVelocityEpsilon = 2.0;
constexpr double kCentralOpenRate = 1.0 / 0.30;
constexpr double kCentralCloseRate = 1.0 / 0.18;
constexpr double kConstellationRevealThreshold = 0.68;
constexpr int kCentreFinalTopMargin = 166;
constexpr int kCentreStartTopMargin = 150;
constexpr int kCentreFinalWidth = 648;
constexpr int kCentreStartWidth = 600;
constexpr int kCentreHeight = 200;
constexpr int kApertureFinalWidth = 610;
constexpr int kApertureStartWidth = 548;
constexpr int kApertureFinalHeight = 162;
constexpr int kApertureStartHeight = 58;
constexpr int kSearchFinalWidth = 360;
constexpr int kSearchStartWidth = 326;
constexpr std::size_t kLauncherIconCacheLimit = 64;
constexpr int kClipboardThumbnailWidth = 104;
constexpr int kClipboardThumbnailHeight = 66;
constexpr std::size_t kClipboardThumbnailCacheLimit = 16;
constexpr std::size_t kClipboardThumbnailMaximumJobs = 2;
constexpr std::size_t kClipboardMaximumDecodedBytes = 6U * 1024U * 1024U;

[[nodiscard]] double clamp_unit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double interval_progress(double value, double start, double end) {
    if (end <= start) return value >= end ? 1.0 : 0.0;
    return clamp_unit((value - start) / (end - start));
}

[[nodiscard]] double smooth_step(double value) {
    const double clamped = clamp_unit(value);
    return clamped * clamped * (3.0 - 2.0 * clamped);
}

[[nodiscard]] double ease_out_cubic(double value) {
    const double inverse = 1.0 - clamp_unit(value);
    return 1.0 - inverse * inverse * inverse;
}

[[nodiscard]] double interpolate(double start, double end, double progress) {
    return start + (end - start) * clamp_unit(progress);
}

bool erase_cliphist_entry_line(std::string& listing, std::string_view id) {
    if (listing.empty() || id.empty()) return false;

    std::size_t line_start = 0;
    while (line_start < listing.size()) {
        const std::size_t line_end = listing.find('\n', line_start);
        const std::size_t content_end = line_end == std::string::npos
            ? listing.size()
            : line_end;
        const std::string_view line(listing.data() + line_start, content_end - line_start);
        if (line.size() > id.size() && line.starts_with(id) && line[id.size()] == '\t') {
            const std::size_t erase_end = line_end == std::string::npos
                ? listing.size()
                : line_end + 1;
            listing.erase(line_start, erase_end - line_start);
            return true;
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }
    return false;
}

fs::path xdg_state_home() {
    if (const char* configured = std::getenv("XDG_STATE_HOME");
        configured != nullptr && *configured != '\0') {
        return fs::path(configured);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".local/state";
    }
    return fs::temp_directory_path() / "realmheart-state";
}

fs::path xdg_config_home() {
    if (const char* configured = std::getenv("XDG_CONFIG_HOME");
        configured != nullptr && *configured != '\0') {
        return fs::path(configured);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / ".config";
    }
    return fs::temp_directory_path() / "realmheart-config";
}

fs::path emoji_script_path() {
    if (const char* configured = std::getenv("REALMHEART_EMOJI_DATA");
        configured != nullptr && *configured != '\0') {
        return fs::path(configured);
    }
    return xdg_config_home() / "hypr/hyprland/scripts/fuzzel-emoji.sh";
}

fs::path constellation_layout_path() {
    return xdg_state_home() / "realmheart/launcher-layout.tsv";
}

struct JpegErrorManager {
    jpeg_error_mgr base{};
    jmp_buf jump_buffer{};
    char message[JMSG_LENGTH_MAX]{};
};

struct JpegDecodeState {
    guchar* pixels = nullptr;
};

extern "C" void launcher_jpeg_error_exit(j_common_ptr common) {
    auto* error = reinterpret_cast<JpegErrorManager*>(common->err);
    (*common->err->format_message)(common, error->message);
    longjmp(error->jump_buffer, 1);
}

[[nodiscard]] bool has_jpeg_signature(const fs::path& path) {
    std::array<unsigned char, 2> signature{};
    std::ifstream stream(path, std::ios::binary);
    return stream.read(
               reinterpret_cast<char*>(signature.data()),
               static_cast<std::streamsize>(signature.size())
           ) &&
           signature[0] == 0xFFU && signature[1] == 0xD8U;
}

[[nodiscard]] GdkTexture* create_cover_texture_from_rgb(
    const guchar* source_pixels,
    int source_width,
    int source_height,
    gsize source_rowstride,
    int target_width,
    int target_height
) {
    if (source_pixels == nullptr || source_width <= 0 || source_height <= 0 ||
        target_width <= 0 || target_height <= 0) {
        return nullptr;
    }

    const gsize target_rowstride = static_cast<gsize>(target_width) * 3U;
    guchar* target_pixels = static_cast<guchar*>(
        g_try_malloc_n(static_cast<gsize>(target_height), target_rowstride)
    );
    if (target_pixels == nullptr) {
        return nullptr;
    }

    const double cover_scale = std::max(
        static_cast<double>(target_width) / static_cast<double>(source_width),
        static_cast<double>(target_height) / static_cast<double>(source_height)
    );
    const double visible_width = static_cast<double>(target_width) / cover_scale;
    const double visible_height = static_cast<double>(target_height) / cover_scale;
    const double source_origin_x =
        (static_cast<double>(source_width) - visible_width) * 0.5;
    const double source_origin_y =
        (static_cast<double>(source_height) - visible_height) * 0.5;

    for (int target_y = 0; target_y < target_height; ++target_y) {
        const double source_y = std::clamp(
            source_origin_y +
                (static_cast<double>(target_y) + 0.5) / cover_scale - 0.5,
            0.0,
            static_cast<double>(source_height - 1)
        );
        const int y0 = static_cast<int>(std::floor(source_y));
        const int y1 = std::min(y0 + 1, source_height - 1);
        const double y_fraction = source_y - static_cast<double>(y0);

        const guchar* row0 =
            source_pixels + static_cast<gsize>(y0) * source_rowstride;
        const guchar* row1 =
            source_pixels + static_cast<gsize>(y1) * source_rowstride;
        guchar* target_row =
            target_pixels + static_cast<gsize>(target_y) * target_rowstride;

        for (int target_x = 0; target_x < target_width; ++target_x) {
            const double source_x = std::clamp(
                source_origin_x +
                    (static_cast<double>(target_x) + 0.5) / cover_scale - 0.5,
                0.0,
                static_cast<double>(source_width - 1)
            );
            const int x0 = static_cast<int>(std::floor(source_x));
            const int x1 = std::min(x0 + 1, source_width - 1);
            const double x_fraction = source_x - static_cast<double>(x0);

            const guchar* pixel00 = row0 + static_cast<gsize>(x0) * 3U;
            const guchar* pixel10 = row0 + static_cast<gsize>(x1) * 3U;
            const guchar* pixel01 = row1 + static_cast<gsize>(x0) * 3U;
            const guchar* pixel11 = row1 + static_cast<gsize>(x1) * 3U;
            guchar* target_pixel = target_row + static_cast<gsize>(target_x) * 3U;

            for (int channel = 0; channel < 3; ++channel) {
                const double upper = std::lerp(
                    static_cast<double>(pixel00[channel]),
                    static_cast<double>(pixel10[channel]),
                    x_fraction
                );
                const double lower = std::lerp(
                    static_cast<double>(pixel01[channel]),
                    static_cast<double>(pixel11[channel]),
                    x_fraction
                );
                target_pixel[channel] = static_cast<guchar>(std::clamp(
                    std::lround(std::lerp(upper, lower, y_fraction)),
                    0L,
                    255L
                ));
            }
        }
    }

    const gsize target_byte_count =
        target_rowstride * static_cast<gsize>(target_height);
    GBytes* bytes = g_bytes_new_take(target_pixels, target_byte_count);
    GdkTexture* texture = gdk_memory_texture_new(
        target_width,
        target_height,
        GDK_MEMORY_R8G8B8,
        bytes,
        target_rowstride
    );
    g_bytes_unref(bytes);
    return texture;
}

[[nodiscard]] GdkTexture* load_jpeg_aperture_texture(
    const fs::path& path,
    int target_width,
    int target_height
) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return nullptr;
    }

    auto* decoder = g_new0(jpeg_decompress_struct, 1);
    auto* error = g_new0(JpegErrorManager, 1);
    auto* state = g_new0(JpegDecodeState, 1);

    decoder->err = jpeg_std_error(&error->base);
    error->base.error_exit = launcher_jpeg_error_exit;

    if (setjmp(error->jump_buffer) != 0) {
        jpeg_destroy_decompress(decoder);
        std::fclose(file);
        g_free(state->pixels);
        if (error->message[0] != '\0') {
            g_warning(
                "Unable to decode launcher JPEG '%s': %s",
                path.c_str(),
                error->message
            );
        }
        g_free(state);
        g_free(error);
        g_free(decoder);
        return nullptr;
    }

    jpeg_create_decompress(decoder);
    jpeg_stdio_src(decoder, file);
    if (jpeg_read_header(decoder, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(decoder);
        std::fclose(file);
        g_free(state);
        g_free(error);
        g_free(decoder);
        return nullptr;
    }

    int scale_denominator = 1;
    for (const int candidate : std::array<int, 3>{8, 4, 2}) {
        const auto scaled_width =
            (decoder->image_width + static_cast<JDIMENSION>(candidate) - 1U) /
            static_cast<JDIMENSION>(candidate);
        const auto scaled_height =
            (decoder->image_height + static_cast<JDIMENSION>(candidate) - 1U) /
            static_cast<JDIMENSION>(candidate);
        if (scaled_width >= static_cast<JDIMENSION>(target_width) &&
            scaled_height >= static_cast<JDIMENSION>(target_height)) {
            scale_denominator = candidate;
            break;
        }
    }

    decoder->scale_num = 1;
    decoder->scale_denom = scale_denominator;
    decoder->out_color_space = JCS_RGB;
    decoder->do_fancy_upsampling = TRUE;
    decoder->do_block_smoothing = TRUE;
    jpeg_start_decompress(decoder);

    if (decoder->output_components != 3U ||
        decoder->output_width > static_cast<JDIMENSION>(std::numeric_limits<int>::max()) ||
        decoder->output_height > static_cast<JDIMENSION>(std::numeric_limits<int>::max())) {
        jpeg_abort_decompress(decoder);
        jpeg_destroy_decompress(decoder);
        std::fclose(file);
        g_free(state);
        g_free(error);
        g_free(decoder);
        return nullptr;
    }

    const int decoded_width = static_cast<int>(decoder->output_width);
    const int decoded_height = static_cast<int>(decoder->output_height);
    const gsize decoded_rowstride = static_cast<gsize>(decoded_width) * 3U;
    state->pixels = static_cast<guchar*>(
        g_try_malloc_n(static_cast<gsize>(decoded_height), decoded_rowstride)
    );
    if (state->pixels == nullptr) {
        jpeg_abort_decompress(decoder);
        jpeg_destroy_decompress(decoder);
        std::fclose(file);
        g_free(state);
        g_free(error);
        g_free(decoder);
        return nullptr;
    }

    while (decoder->output_scanline < decoder->output_height) {
        JSAMPROW row = state->pixels +
            static_cast<gsize>(decoder->output_scanline) * decoded_rowstride;
        if (jpeg_read_scanlines(decoder, &row, 1) != 1U) {
            jpeg_abort_decompress(decoder);
            jpeg_destroy_decompress(decoder);
            std::fclose(file);
            g_free(state->pixels);
            g_free(state);
            g_free(error);
            g_free(decoder);
            return nullptr;
        }
    }

    jpeg_finish_decompress(decoder);
    jpeg_destroy_decompress(decoder);
    std::fclose(file);
    g_free(error);
    g_free(decoder);

    GdkTexture* texture = create_cover_texture_from_rgb(
        state->pixels,
        decoded_width,
        decoded_height,
        decoded_rowstride,
        target_width,
        target_height
    );
    g_free(state->pixels);
    g_free(state);
    return texture;
}

[[nodiscard]] GdkTexture* load_aperture_texture(
    const fs::path& path,
    int logical_width,
    int logical_height,
    int scale_factor
) {
    const int target_width = std::max(1, logical_width * scale_factor);
    const int target_height = std::max(1, logical_height * scale_factor);

    // GTK 4's generic image loader routes JPEG decoding through glycin. That
    // initializes a persistent sandbox/worker pool on first launcher use. Decode
    // JPEG wallpapers directly with libjpeg-turbo instead; non-JPEG formats keep
    // the generic fallback so supported wallpaper formats do not regress.
    if (has_jpeg_signature(path)) {
        return load_jpeg_aperture_texture(path, target_width, target_height);
    }

    int source_width = 0;
    int source_height = 0;
    if (gdk_pixbuf_get_file_info(
            path.c_str(),
            &source_width,
            &source_height
        ) == nullptr ||
        source_width <= 0 || source_height <= 0) {
        return nullptr;
    }
    const double cover_scale = std::max(
        static_cast<double>(target_width) / static_cast<double>(source_width),
        static_cast<double>(target_height) / static_cast<double>(source_height)
    );
    const int decoded_width = std::max(
        target_width,
        static_cast<int>(std::ceil(static_cast<double>(source_width) * cover_scale))
    );
    const int decoded_height = std::max(
        target_height,
        static_cast<int>(std::ceil(static_cast<double>(source_height) * cover_scale))
    );

    GError* error = nullptr;
    GdkPixbuf* decoded = gdk_pixbuf_new_from_file_at_scale(
        path.c_str(),
        decoded_width,
        decoded_height,
        TRUE,
        &error
    );
    if (decoded == nullptr) {
        if (error != nullptr) {
            g_warning(
                "Unable to decode launcher wallpaper '%s': %s",
                path.c_str(),
                error->message
            );
            g_error_free(error);
        }
        return nullptr;
    }

    const int actual_width = gdk_pixbuf_get_width(decoded);
    const int actual_height = gdk_pixbuf_get_height(decoded);
    if (actual_width < target_width || actual_height < target_height) {
        g_object_unref(decoded);
        return nullptr;
    }

    GdkPixbuf* cropped = gdk_pixbuf_new(
        GDK_COLORSPACE_RGB,
        gdk_pixbuf_get_has_alpha(decoded),
        8,
        target_width,
        target_height
    );
    if (cropped == nullptr) {
        g_object_unref(decoded);
        return nullptr;
    }

    const int source_x = (actual_width - target_width) / 2;
    const int source_y = (actual_height - target_height) / 2;
    gdk_pixbuf_copy_area(
        decoded,
        source_x,
        source_y,
        target_width,
        target_height,
        cropped,
        0,
        0
    );
    g_object_unref(decoded);

    const gsize rowstride = static_cast<gsize>(
        gdk_pixbuf_get_rowstride(cropped)
    );
    const gsize byte_count = rowstride * static_cast<gsize>(target_height);
    GBytes* pixels = g_bytes_new(
        gdk_pixbuf_get_pixels(cropped),
        byte_count
    );
    const GdkMemoryFormat format = gdk_pixbuf_get_has_alpha(cropped)
        ? GDK_MEMORY_R8G8B8A8
        : GDK_MEMORY_R8G8B8;
    GdkTexture* texture = gdk_memory_texture_new(
        target_width,
        target_height,
        format,
        pixels,
        rowstride
    );
    g_bytes_unref(pixels);
    g_object_unref(cropped);
    return texture;
}

[[nodiscard]] GdkTexture* memory_texture_from_pixbuf(GdkPixbuf* pixbuf) {
    if (pixbuf == nullptr) return nullptr;

    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    if (width <= 0 || height <= 0 || rowstride <= 0) return nullptr;

    const gsize byte_count = static_cast<gsize>(rowstride) *
        static_cast<gsize>(height);
    GBytes* pixels = g_bytes_new(gdk_pixbuf_get_pixels(pixbuf), byte_count);
    const GdkMemoryFormat format = gdk_pixbuf_get_has_alpha(pixbuf)
        ? GDK_MEMORY_R8G8B8A8
        : GDK_MEMORY_R8G8B8;
    GdkTexture* texture = gdk_memory_texture_new(
        width,
        height,
        format,
        pixels,
        static_cast<gsize>(rowstride)
    );
    g_bytes_unref(pixels);
    return texture;
}

struct DecodedClipboardThumbnail {
    std::vector<guchar> pixels;
    int width = 0;
    int height = 0;
    int rowstride = 0;
    int source_width = 0;
    int source_height = 0;
    bool has_alpha = false;
    std::string format;
};

struct ClipboardLoaderSizing {
    int source_width = 0;
    int source_height = 0;
};

void prepare_clipboard_thumbnail_size(
    GdkPixbufLoader* loader,
    int width,
    int height,
    gpointer user_data
) {
    auto* sizing = static_cast<ClipboardLoaderSizing*>(user_data);
    sizing->source_width = width;
    sizing->source_height = height;
    if (width <= 0 || height <= 0) return;

    const double scale = std::min({
        1.0,
        static_cast<double>(kClipboardThumbnailWidth) / static_cast<double>(width),
        static_cast<double>(kClipboardThumbnailHeight) / static_cast<double>(height),
    });
    const int target_width = std::max(
        1,
        static_cast<int>(std::lround(static_cast<double>(width) * scale))
    );
    const int target_height = std::max(
        1,
        static_cast<int>(std::lround(static_cast<double>(height) * scale))
    );
    gdk_pixbuf_loader_set_size(loader, target_width, target_height);
}

std::optional<DecodedClipboardThumbnail> decode_clipboard_thumbnail(
    std::string_view bytes
) {
    if (bytes.empty() || bytes.size() > kClipboardMaximumDecodedBytes) {
        return std::nullopt;
    }

    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    ClipboardLoaderSizing sizing;
    g_signal_connect(
        loader,
        "size-prepared",
        G_CALLBACK(prepare_clipboard_thumbnail_size),
        &sizing
    );

    GError* error = nullptr;
    const bool wrote = gdk_pixbuf_loader_write(
        loader,
        reinterpret_cast<const guchar*>(bytes.data()),
        bytes.size(),
        &error
    );
    const bool closed = wrote && gdk_pixbuf_loader_close(loader, &error);
    if (!closed) {
        g_clear_error(&error);
        g_object_unref(loader);
        return std::nullopt;
    }

    GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf == nullptr) {
        g_object_unref(loader);
        return std::nullopt;
    }
    g_object_ref(pixbuf);

    DecodedClipboardThumbnail decoded;
    decoded.width = gdk_pixbuf_get_width(pixbuf);
    decoded.height = gdk_pixbuf_get_height(pixbuf);
    decoded.rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    decoded.source_width = sizing.source_width > 0 ? sizing.source_width : decoded.width;
    decoded.source_height = sizing.source_height > 0 ? sizing.source_height : decoded.height;
    decoded.has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);

    if (GdkPixbufFormat* format = gdk_pixbuf_loader_get_format(loader); format != nullptr) {
        if (gchar* name = gdk_pixbuf_format_get_name(format); name != nullptr) {
            decoded.format = name;
            g_free(name);
        }
    }

    if (decoded.width <= 0 || decoded.height <= 0 || decoded.rowstride <= 0) {
        g_object_unref(pixbuf);
        g_object_unref(loader);
        return std::nullopt;
    }

    const gsize byte_count = static_cast<gsize>(decoded.rowstride) *
        static_cast<gsize>(decoded.height);
    decoded.pixels.assign(
        gdk_pixbuf_get_pixels(pixbuf),
        gdk_pixbuf_get_pixels(pixbuf) + byte_count
    );

    g_object_unref(pixbuf);
    g_object_unref(loader);
    return decoded;
}

std::string uppercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char character) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    });
    if (value == "JPG") return "JPEG";
    return value;
}

const char* result_kind_label(services::LauncherResultKind kind) {
    switch (kind) {
    case services::LauncherResultKind::Application:
        return "APPLICATION";
    case services::LauncherResultKind::Command:
        return "COMMAND";
    case services::LauncherResultKind::Calculation:
        return "CALCULATION";
    case services::LauncherResultKind::Action:
        return "REALMHEART ACTION";
    case services::LauncherResultKind::Emoji:
        return "EMOJI";
    case services::LauncherResultKind::Clipboard:
        return "CLIPBOARD";
    case services::LauncherResultKind::ClipboardAction:
        return "CLIPBOARD ACTION";
    case services::LauncherResultKind::LauncherCommand:
        return "LAUNCHER COMMAND";
    }
    return "TARGET";
}

void clear_list(GtkWidget* list) {
    GtkWidget* child = gtk_widget_get_first_child(list);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(list), child);
        child = next;
    }
}

void clear_fixed(GtkWidget* fixed) {
    GtkWidget* child = gtk_widget_get_first_child(fixed);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_fixed_remove(GTK_FIXED(fixed), child);
        child = next;
    }
}

} // namespace

LauncherOverlay::ResultRowMotion::~ResultRowMotion() {
    if (row != nullptr) {
        g_object_remove_weak_pointer(
            G_OBJECT(row),
            reinterpret_cast<gpointer*>(&row)
        );
    }
    if (content != nullptr) {
        g_object_remove_weak_pointer(
            G_OBJECT(content),
            reinterpret_cast<gpointer*>(&content)
        );
    }
}

void LauncherOverlay::ResultRowMotion::bind(
    GtkListBoxRow* new_row,
    GtkWidget* new_content
) {
    row = new_row;
    content = new_content;
    if (row != nullptr) {
        g_object_add_weak_pointer(
            G_OBJECT(row),
            reinterpret_cast<gpointer*>(&row)
        );
    }
    if (content != nullptr) {
        g_object_add_weak_pointer(
            G_OBJECT(content),
            reinterpret_cast<gpointer*>(&content)
        );
    }
}

LauncherOverlay::LauncherOverlay(
    GtkApplication* app,
    services::LauncherService& service,
    services::WallpaperService& wallpaper_service,
    CommandReceiptOverlay& command_receipts
) : service_(service),
    wallpaper_service_(wallpaper_service),
    command_receipts_(command_receipts) {
    clipboard_async_state_ = std::make_shared<ClipboardAsyncState>();
    clipboard_async_state_->owner.store(this, std::memory_order_release);
    emoji_async_state_ = std::make_shared<EmojiAsyncState>();
    emoji_async_state_->owner.store(this, std::memory_order_release);

    window_ = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(window_), "realmheart-launcher-window");

    setup_window();
    setup_ui();
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

LauncherOverlay::~LauncherOverlay() {
    if (clipboard_async_state_) {
        clipboard_async_state_->owner.store(nullptr, std::memory_order_release);
        clipboard_async_state_->generation.fetch_add(1, std::memory_order_acq_rel);
    }
    if (clipboard_thumbnail_visibility_idle_id_ != 0) {
        g_source_remove(clipboard_thumbnail_visibility_idle_id_);
        clipboard_thumbnail_visibility_idle_id_ = 0;
    }
    if (clipboard_page_growth_idle_id_ != 0) {
        g_source_remove(clipboard_page_growth_idle_id_);
        clipboard_page_growth_idle_id_ = 0;
    }
    if (emoji_async_state_) {
        emoji_async_state_->owner.store(nullptr, std::memory_order_release);
        emoji_async_state_->generation.fetch_add(1, std::memory_order_acq_rel);
    }
    if (emoji_page_growth_idle_id_ != 0) {
        g_source_remove(emoji_page_growth_idle_id_);
        emoji_page_growth_idle_id_ = 0;
    }
    clear_clipboard_thumbnail_cache();

    if (central_tick_id_ != 0 && root_ != nullptr) {
        gtk_widget_remove_tick_callback(root_, central_tick_id_);
        central_tick_id_ = 0;
    }
    if (constellation_tick_id_ != 0 && root_ != nullptr) {
        gtk_widget_remove_tick_callback(root_, constellation_tick_id_);
        constellation_tick_id_ = 0;
    }
    if (result_selection_tick_id_ != 0 && root_ != nullptr) {
        gtk_widget_remove_tick_callback(root_, result_selection_tick_id_);
        result_selection_tick_id_ = 0;
    }
    for (auto& node : constellation_nodes_) {
        if (node->menu != nullptr && gtk_widget_get_parent(node->menu) != nullptr) {
            gtk_widget_unparent(node->menu);
            node->menu = nullptr;
        }
    }
    command_receipts_.detach();
    if (launcher_icon_theme_ != nullptr &&
        launcher_icon_theme_changed_handler_ != 0) {
        g_signal_handler_disconnect(
            launcher_icon_theme_,
            launcher_icon_theme_changed_handler_
        );
        launcher_icon_theme_changed_handler_ = 0;
        launcher_icon_theme_ = nullptr;
    }
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
    }
    clear_launcher_icon_cache();
}

void LauncherOverlay::setup_window() {
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-launcher";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    spec.anchor_left = true;
    spec.anchor_right = true;
    apply_layer_surface(window_, spec);

    gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
}

void LauncherOverlay::setup_ui() {
    root_ = gtk_overlay_new();
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);
    gtk_widget_add_css_class(root_, "realmheart-launcher-root");

    dismiss_ = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(dismiss_), FALSE);
    gtk_widget_set_hexpand(dismiss_, TRUE);
    gtk_widget_set_vexpand(dismiss_, TRUE);
    gtk_widget_add_css_class(dismiss_, "realmheart-launcher-dismiss");
    g_signal_connect(dismiss_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        static_cast<LauncherOverlay*>(data)->hide();
    }), this);
    gtk_overlay_set_child(GTK_OVERLAY(root_), dismiss_);

    // The constellation is a full-screen freeform canvas, but the canvas itself
    // is not a pointer target. Only its app nodes intercept clicks, so clicking
    // empty launcher space still reaches the dismiss surface underneath.
    constellation_canvas_ = gtk_fixed_new();
    gtk_widget_set_hexpand(constellation_canvas_, TRUE);
    gtk_widget_set_vexpand(constellation_canvas_, TRUE);
    gtk_widget_set_halign(constellation_canvas_, GTK_ALIGN_FILL);
    gtk_widget_set_valign(constellation_canvas_, GTK_ALIGN_FILL);
    // GtkFixed must remain targetable: disabling it also removes its children
    // from pointer picking, which made every constellation node decorative.
    gtk_widget_set_can_target(constellation_canvas_, TRUE);
    gtk_widget_add_css_class(constellation_canvas_, "realmheart-launcher-constellation");
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), constellation_canvas_);

    GtkGesture* canvas_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(canvas_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(canvas_click, "released", G_CALLBACK(+[](
        GtkGestureClick*, int, double x, double y, gpointer data
    ) {
        auto* overlay = static_cast<LauncherOverlay*>(data);
        if (!overlay->point_hits_constellation_node(x, y)) overlay->hide();
    }), this);
    gtk_widget_add_controller(
        constellation_canvas_,
        GTK_EVENT_CONTROLLER(canvas_click)
    );

    g_signal_connect(root_, "notify::width", G_CALLBACK(+[](
        GObject*, GParamSpec*, gpointer data
    ) {
        static_cast<LauncherOverlay*>(data)->layout_constellation();
    }), this);
    g_signal_connect(root_, "notify::height", G_CALLBACK(+[](
        GObject*, GParamSpec*, gpointer data
    ) {
        static_cast<LauncherOverlay*>(data)->layout_constellation();
    }), this);

    // Centre: only the wallpaper aperture and search instrument remain in the
    // idle shell. Search results still unfold in their narrower lower surface.
    centre_column_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_size_request(centre_column_, kCentreFinalWidth, -1);
    gtk_widget_set_halign(centre_column_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(centre_column_, GTK_ALIGN_START);
    gtk_widget_set_hexpand(centre_column_, FALSE);
    gtk_widget_set_vexpand(centre_column_, FALSE);
    gtk_widget_set_margin_top(centre_column_, kCentreFinalTopMargin);
    gtk_widget_add_css_class(centre_column_, "realmheart-launcher-centre-column");

    // Keep the outer instrument at its final height while the wallpaper itself
    // opens from a centred slit. This makes the motion read as an aperture,
    // rather than as another panel unfurling from an edge.
    centre_shell_ = gtk_overlay_new();
    gtk_widget_set_size_request(centre_shell_, kCentreFinalWidth, kCentreHeight);
    gtk_widget_set_halign(centre_shell_, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(centre_shell_, FALSE);
    gtk_widget_add_css_class(centre_shell_, "realmheart-launcher-centre-shell");

    wallpaper_frame_ = gtk_overlay_new();
    gtk_widget_set_size_request(
        wallpaper_frame_,
        kApertureFinalWidth,
        kApertureFinalHeight
    );
    gtk_widget_set_halign(wallpaper_frame_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(wallpaper_frame_, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(wallpaper_frame_, FALSE);
    gtk_widget_set_vexpand(wallpaper_frame_, FALSE);
    gtk_widget_add_css_class(wallpaper_frame_, "realmheart-launcher-wallpaper-frame");
    gtk_widget_set_overflow(wallpaper_frame_, GTK_OVERFLOW_HIDDEN);

    wallpaper_picture_ = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(wallpaper_picture_), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(wallpaper_picture_), TRUE);
    gtk_widget_set_hexpand(wallpaper_picture_, TRUE);
    gtk_widget_set_vexpand(wallpaper_picture_, TRUE);
    gtk_widget_add_css_class(wallpaper_picture_, "realmheart-launcher-wallpaper");

    // GtkPicture reports the wallpaper's full intrinsic size. A non-propagating
    // viewport keeps that natural size from inflating the launcher geometry.
    GtkWidget* wallpaper_viewport = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(wallpaper_viewport),
        GTK_POLICY_NEVER,
        GTK_POLICY_NEVER
    );
    gtk_scrolled_window_set_propagate_natural_width(
        GTK_SCROLLED_WINDOW(wallpaper_viewport),
        FALSE
    );
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(wallpaper_viewport),
        FALSE
    );
    gtk_widget_set_hexpand(wallpaper_viewport, TRUE);
    gtk_widget_set_vexpand(wallpaper_viewport, TRUE);
    gtk_widget_add_css_class(wallpaper_viewport, "realmheart-launcher-wallpaper-viewport");
    gtk_scrolled_window_set_child(
        GTK_SCROLLED_WINDOW(wallpaper_viewport),
        wallpaper_picture_
    );
    gtk_overlay_set_child(GTK_OVERLAY(wallpaper_frame_), wallpaper_viewport);

    GtkWidget* wallpaper_shade = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(wallpaper_shade, GTK_ALIGN_FILL);
    gtk_widget_set_valign(wallpaper_shade, GTK_ALIGN_END);
    gtk_widget_set_size_request(wallpaper_shade, -1, 110);
    gtk_widget_set_can_target(wallpaper_shade, FALSE);
    gtk_widget_add_css_class(wallpaper_shade, "realmheart-launcher-wallpaper-shade");
    gtk_overlay_add_overlay(GTK_OVERLAY(wallpaper_frame_), wallpaper_shade);

    search_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search_entry_),
        "Search apps, calculate, or run a command"
    );
    gtk_widget_set_size_request(search_entry_, 360, 50);
    gtk_widget_set_halign(search_entry_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(search_entry_, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(search_entry_, "realmheart-launcher-search");
    g_signal_connect(search_entry_, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
        static_cast<LauncherOverlay*>(data)->on_search_changed();
    }), this);
    g_signal_connect(search_entry_, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
        static_cast<LauncherOverlay*>(data)->activate_selected();
    }), this);
    gtk_overlay_add_overlay(GTK_OVERLAY(wallpaper_frame_), search_entry_);

    GtkWidget* centre_surface = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(centre_surface, TRUE);
    gtk_widget_set_vexpand(centre_surface, TRUE);
    gtk_widget_set_can_target(centre_surface, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(centre_shell_), centre_surface);
    gtk_overlay_add_overlay(GTK_OVERLAY(centre_shell_), wallpaper_frame_);
    gtk_overlay_set_clip_overlay(
        GTK_OVERLAY(centre_shell_),
        wallpaper_frame_,
        TRUE
    );

    activation_sweep_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(activation_sweep_, 92, 2);
    gtk_widget_set_halign(activation_sweep_, GTK_ALIGN_START);
    gtk_widget_set_valign(activation_sweep_, GTK_ALIGN_END);
    gtk_widget_set_can_target(activation_sweep_, FALSE);
    gtk_widget_set_visible(activation_sweep_, FALSE);
    gtk_widget_add_css_class(
        activation_sweep_,
        "realmheart-launcher-activation-sweep"
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(centre_shell_), activation_sweep_);

    gtk_box_append(GTK_BOX(centre_column_), centre_shell_);

    results_revealer_ = gtk_revealer_new();
    gtk_revealer_set_transition_type(
        GTK_REVEALER(results_revealer_),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN
    );
    gtk_revealer_set_transition_duration(GTK_REVEALER(results_revealer_), 170);
    gtk_widget_set_margin_top(results_revealer_, 2);

    GtkWidget* results_shell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(results_shell, 520, -1);
    gtk_widget_set_halign(results_shell, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(results_shell, "realmheart-launcher-results-shell");

    results_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(results_list_), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(results_list_), FALSE);
    gtk_widget_add_css_class(results_list_, "realmheart-launcher-results-list");
    g_signal_connect(results_list_, "row-selected", G_CALLBACK(+[](
        GtkListBox*, GtkListBoxRow* row, gpointer data
    ) {
        static_cast<LauncherOverlay*>(data)->on_result_selected(row);
    }), this);
    g_signal_connect(results_list_, "row-activated", G_CALLBACK(+[](
        GtkListBox*, GtkListBoxRow* row, gpointer data
    ) {
        const int index = gtk_list_box_row_get_index(row);
        if (index >= 0) {
            static_cast<LauncherOverlay*>(data)->activate_result(
                static_cast<std::size_t>(index)
            );
        }
    }), this);

    results_scroller_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(results_scroller_),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC
    );
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(results_scroller_),
        TRUE
    );
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(results_scroller_),
        kNormalResultsMaximumHeight
    );
    gtk_scrolled_window_set_child(
        GTK_SCROLLED_WINDOW(results_scroller_),
        results_list_
    );
    GtkAdjustment* results_adjustment = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(results_scroller_)
    );
    const auto on_results_adjustment_changed = +[](GtkAdjustment*, gpointer data) {
        auto* overlay = static_cast<LauncherOverlay*>(data);
        overlay->schedule_visible_clipboard_thumbnails();
        overlay->schedule_clipboard_page_growth();
        overlay->schedule_emoji_page_growth();
        if (overlay->selected_result_row_ != nullptr) {
            overlay->retarget_result_selection(overlay->selected_result_row_);
        }
    };
    g_signal_connect(
        results_adjustment,
        "value-changed",
        G_CALLBACK(on_results_adjustment_changed),
        this
    );
    g_signal_connect(
        results_adjustment,
        "changed",
        G_CALLBACK(on_results_adjustment_changed),
        this
    );

    results_overlay_ = gtk_overlay_new();
    gtk_widget_set_hexpand(results_overlay_, TRUE);
    gtk_widget_set_vexpand(results_overlay_, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(results_overlay_), results_scroller_);

    result_selection_indicator_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(result_selection_indicator_, GTK_ALIGN_START);
    gtk_widget_set_valign(result_selection_indicator_, GTK_ALIGN_START);
    gtk_widget_set_can_target(result_selection_indicator_, FALSE);
    gtk_widget_set_visible(result_selection_indicator_, FALSE);
    gtk_widget_add_css_class(
        result_selection_indicator_,
        "realmheart-launcher-result-selection"
    );
    gtk_overlay_add_overlay(
        GTK_OVERLAY(results_overlay_),
        result_selection_indicator_
    );
    gtk_overlay_set_clip_overlay(
        GTK_OVERLAY(results_overlay_),
        result_selection_indicator_,
        TRUE
    );

    gtk_box_append(GTK_BOX(results_shell), results_overlay_);
    gtk_revealer_set_child(GTK_REVEALER(results_revealer_), results_shell);
    gtk_box_append(GTK_BOX(centre_column_), results_revealer_);

    // The centre is layered above the constellation. Nodes are constrained to
    // the lower interaction region, but this also protects the search surface
    // from a malformed or hand-edited saved position.
    gtk_overlay_add_overlay(GTK_OVERLAY(root_), centre_column_);

    // Command feedback belongs to the same fullscreen launcher surface. It is
    // layered above the centre/constellation and never creates a notification-
    // style Wayland window of its own.
    command_receipts_.attach(GTK_OVERLAY(root_));

    gtk_window_set_child(window_, root_);

    GtkEventController* key_controller = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_controller, GTK_PHASE_CAPTURE);
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(+[](
        GtkEventControllerKey*, guint keyval, guint, GdkModifierType modifiers, gpointer data
    ) -> gboolean {
        return static_cast<LauncherOverlay*>(data)->handle_key(keyval, modifiers);
    }), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), key_controller);
}

void LauncherOverlay::refresh_wallpaper() {
    const auto path = wallpaper_service_.load_path();
    if (!path) {
        gtk_picture_set_paintable(GTK_PICTURE(wallpaper_picture_), nullptr);
        wallpaper_texture_path_.clear();
        wallpaper_texture_scale_factor_ = 0;
        gtk_widget_add_css_class(
            wallpaper_picture_,
            "realmheart-launcher-wallpaper-missing"
        );
        return;
    }

    const int scale_factor = std::max(
        1,
        gtk_widget_get_scale_factor(wallpaper_picture_)
    );
    if (wallpaper_texture_path_ == *path &&
        wallpaper_texture_scale_factor_ == scale_factor &&
        gtk_picture_get_paintable(GTK_PICTURE(wallpaper_picture_)) != nullptr) {
        return;
    }

    GdkTexture* texture = load_aperture_texture(
        *path,
        kApertureFinalWidth,
        kApertureFinalHeight,
        scale_factor
    );
    if (texture == nullptr) {
        // Preserve compatibility with an unusual image format even though this
        // fallback may retain a full-resolution decode. Common JPEG/PNG/WebP
        // wallpapers take the scaled path above.
        GFile* file = g_file_new_for_path(path->c_str());
        gtk_picture_set_file(GTK_PICTURE(wallpaper_picture_), file);
        g_object_unref(file);
        wallpaper_texture_path_.clear();
        wallpaper_texture_scale_factor_ = 0;
    } else {
        gtk_picture_set_paintable(
            GTK_PICTURE(wallpaper_picture_),
            GDK_PAINTABLE(texture)
        );
        g_object_unref(texture);
        wallpaper_texture_path_ = *path;
        wallpaper_texture_scale_factor_ = scale_factor;
    }

    gtk_widget_remove_css_class(
        wallpaper_picture_,
        "realmheart-launcher-wallpaper-missing"
    );
}

void LauncherOverlay::refresh_idle_content() {
    load_constellation_layout();
    if (!constellation_layout_loaded_) {
        seed_constellation_layout();
    }
    rebuild_constellation();
    set_selected_result(nullptr);
}

void LauncherOverlay::clear_launcher_icon_cache() {
    for (const auto& entry : launcher_icon_textures_) {
        g_object_unref(entry.second);
    }
    launcher_icon_textures_.clear();
}

GdkTexture* LauncherOverlay::launcher_icon_texture(
    std::string_view requested_icon_name,
    int logical_pixels
) {
    GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window_));
    if (display == nullptr) return nullptr;

    GtkIconTheme* icon_theme = gtk_icon_theme_get_for_display(display);
    if (icon_theme == nullptr) return nullptr;

    if (launcher_icon_theme_ != icon_theme) {
        if (launcher_icon_theme_ != nullptr &&
            launcher_icon_theme_changed_handler_ != 0) {
            g_signal_handler_disconnect(
                launcher_icon_theme_,
                launcher_icon_theme_changed_handler_
            );
        }
        clear_launcher_icon_cache();
        launcher_icon_theme_ = icon_theme;
        launcher_icon_theme_changed_handler_ = g_signal_connect(
            icon_theme,
            "changed",
            G_CALLBACK(+[](GtkIconTheme*, gpointer data) {
                static_cast<LauncherOverlay*>(data)->clear_launcher_icon_cache();
            }),
            this
        );
    }

    const int size = std::clamp(logical_pixels, 16, 96);
    const int scale = std::max(1, gtk_widget_get_scale_factor(GTK_WIDGET(window_)));
    const std::string icon_name = requested_icon_name.empty()
        ? "application-x-executable"
        : std::string(requested_icon_name);
    const std::string cache_key = icon_name + '\n' +
        std::to_string(size) + '@' + std::to_string(scale);

    const auto cached = launcher_icon_textures_.find(cache_key);
    if (cached != launcher_icon_textures_.end()) return cached->second;

    const char* fallbacks[] = {"application-x-executable", nullptr};
    GtkIconPaintable* paintable = gtk_icon_theme_lookup_icon(
        icon_theme,
        icon_name.c_str(),
        fallbacks,
        size,
        scale,
        GTK_TEXT_DIR_NONE,
        GTK_ICON_LOOKUP_FORCE_REGULAR
    );
    if (paintable == nullptr) return nullptr;

    // get_file() returns an owned GFile reference. Keep that reference while
    // discarding the lookup paintable before decoding the icon ourselves.
    GFile* icon_file = gtk_icon_paintable_get_file(paintable);
    g_object_unref(paintable);
    if (icon_file == nullptr) return nullptr;

    GError* error = nullptr;
    GFileInputStream* file_stream = g_file_read(icon_file, nullptr, &error);
    g_object_unref(icon_file);
    if (file_stream == nullptr) {
        g_clear_error(&error);
        return nullptr;
    }

    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_stream_at_scale(
        G_INPUT_STREAM(file_stream),
        size,
        size,
        TRUE,
        nullptr,
        &error
    );
    static_cast<void>(g_input_stream_close(
        G_INPUT_STREAM(file_stream),
        nullptr,
        nullptr
    ));
    g_object_unref(file_stream);
    if (pixbuf == nullptr) {
        g_clear_error(&error);
        return nullptr;
    }

    GdkTexture* texture = memory_texture_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    if (texture == nullptr) return nullptr;

    // The cache is deliberately small and stores only aperture-sized icon
    // textures. Clearing it is safe: GtkPicture keeps its own reference to any
    // texture currently visible in the constellation or result list.
    if (launcher_icon_textures_.size() >= kLauncherIconCacheLimit) {
        clear_launcher_icon_cache();
    }
    launcher_icon_textures_.emplace(cache_key, texture);
    return texture;
}

GtkWidget* LauncherOverlay::make_launcher_icon(
    std::string_view icon_name,
    int logical_pixels
) {
    // Realmheart-owned SVGs preserve their semantic primary/accent colors.
    // Route them through ThemedSvgIcon instead of flattening them through the
    // desktop icon theme and generic image loader.
    if (icon_name.ends_with(".svg") && icon_name.find('/') != std::string_view::npos) {
        bar::widgets::ThemedSvgIcon themed_icon(
            std::string(icon_name),
            logical_pixels
        );
        GtkWidget* widget = themed_icon.widget();
        gtk_widget_set_can_target(widget, FALSE);
        gtk_widget_add_css_class(widget, "realmheart-launcher-app-icon");
        return widget;
    }

    GdkTexture* texture = launcher_icon_texture(icon_name, logical_pixels);
    if (texture != nullptr) {
        GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
        gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
        gtk_widget_set_size_request(picture, logical_pixels, logical_pixels);
        gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(picture, GTK_ALIGN_CENTER);
        gtk_widget_set_can_target(picture, FALSE);
        gtk_widget_add_css_class(picture, "realmheart-launcher-app-icon");
        return picture;
    }

    // A missing or unusual icon should not fall back into GtkImage's generic
    // glycin path. Realmheart's own lightweight SVG renderer provides a stable
    // placeholder without creating another loader worker pool.
    bar::widgets::ThemedSvgIcon fallback(
        "Realmheart-Icons/app-generic.svg",
        logical_pixels
    );
    GtkWidget* widget = fallback.widget();
    gtk_widget_set_can_target(widget, FALSE);
    gtk_widget_add_css_class(widget, "realmheart-launcher-app-icon");
    return widget;
}

void LauncherOverlay::load_constellation_layout() {
    if (constellation_layout_loaded_) return;

    const fs::path path = constellation_layout_path();
    std::error_code error;
    if (!fs::exists(path, error) || error) {
        return;
    }

    constellation_layout_loaded_ = true;
    constellation_layout_.clear();

    std::ifstream input(path);
    std::string application_id;
    double normalized_x = 0.5;
    double normalized_y = 0.5;
    while (input >> std::quoted(application_id) >> normalized_x >> normalized_y) {
        if (application_id.empty()) continue;
        constellation_layout_.push_back({
            application_id,
            std::clamp(normalized_x, 0.0, 1.0),
            std::clamp(normalized_y, 0.0, 1.0),
        });
        if (constellation_layout_.size() >= kMaximumConstellationApplications) break;
    }
}

void LauncherOverlay::save_constellation_layout() const {
    const fs::path path = constellation_layout_path();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return;

    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return;

    output << std::setprecision(8);
    for (const auto& placement : constellation_layout_) {
        output << std::quoted(placement.application_id) << '\t'
               << placement.normalized_x << '\t'
               << placement.normalized_y << '\n';
    }
    output.close();
    if (!output) {
        fs::remove(temporary, error);
        return;
    }

    fs::rename(temporary, path, error);
    if (error) {
        error.clear();
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
    }
}

void LauncherOverlay::seed_constellation_layout() {
    constellation_layout_.clear();
    const auto candidates = service_.recommendations(kMaximumConstellationApplications);
    for (const auto& candidate : candidates) {
        if (candidate.kind != services::LauncherResultKind::Application) continue;
        const auto [x, y] = default_constellation_position(constellation_layout_.size());
        constellation_layout_.push_back({candidate.id, x, y});
        if (constellation_layout_.size() >= kSeedApplicationCount) break;
    }
    constellation_layout_loaded_ = true;
    save_constellation_layout();
}

std::pair<double, double> LauncherOverlay::default_constellation_position(
    std::size_t index
) const {
    static constexpr std::array<std::pair<double, double>, 12> positions{{
        {0.29, 0.04},
        {0.62, 0.02},
        {0.46, 0.25},
        {0.73, 0.39},
        {0.17, 0.35},
        {0.55, 0.58},
        {0.82, 0.20},
        {0.34, 0.72},
        {0.08, 0.62},
        {0.68, 0.78},
        {0.88, 0.57},
        {0.47, 0.90},
    }};
    return positions[index % positions.size()];
}

void LauncherOverlay::rebuild_constellation() {
    hovered_constellation_node_ = nullptr;
    clear_constellation_selection();
    for (auto& node : constellation_nodes_) {
        if (node->menu != nullptr && gtk_widget_get_parent(node->menu) != nullptr) {
            gtk_widget_unparent(node->menu);
            node->menu = nullptr;
        }
    }
    clear_fixed(constellation_canvas_);
    constellation_nodes_.clear();

    // A single focus reticle moves between spatially selected applications.
    // Keeping it as a separate child lets keyboard navigation read as one
    // continuous motion instead of abruptly repainting unrelated cards.
    selection_indicator_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(
        selection_indicator_,
        kSelectionIndicatorWidth,
        kSelectionIndicatorHeight
    );
    gtk_widget_set_can_target(selection_indicator_, FALSE);
    gtk_widget_set_focusable(selection_indicator_, FALSE);
    gtk_widget_set_opacity(selection_indicator_, 0.0);
    gtk_widget_add_css_class(
        selection_indicator_,
        "realmheart-launcher-constellation-cursor"
    );
    gtk_fixed_put(
        GTK_FIXED(constellation_canvas_),
        selection_indicator_,
        0.0,
        0.0
    );
    selection_indicator_x_ = 0.0;
    selection_indicator_y_ = 0.0;
    selection_indicator_velocity_x_ = 0.0;
    selection_indicator_velocity_y_ = 0.0;
    selection_indicator_opacity_ = 0.0;
    selection_indicator_initialized_ = false;
    selection_indicator_target_visible_ = false;

    std::vector<ConstellationPlacement> valid_layout;
    valid_layout.reserve(constellation_layout_.size());

    for (const auto& placement : constellation_layout_) {
        const auto application = service_.application_by_id(placement.application_id);
        if (!application.has_value()) continue;

        auto node = std::make_unique<ConstellationNode>();
        node->result = *application;
        node->normalized_x = placement.normalized_x;
        node->normalized_y = placement.normalized_y;
        node->opacity = constellation_target_visible_ ? 1.0 : 0.0;

        // Use a plain event surface instead of GtkButton. GtkButton installs
        // its own click gesture, which competes with a freeform drag gesture.
        GtkWidget* button = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        node->widget = button;
        gtk_widget_set_size_request(button, kConstellationNodeWidth, kConstellationNodeHeight);
        gtk_widget_set_focusable(button, TRUE);
        gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        gtk_widget_set_cursor_from_name(button, "grab");
        gtk_widget_set_opacity(button, node->opacity);
        gtk_widget_set_can_target(button, constellation_target_visible_);
        gtk_widget_add_css_class(button, "realmheart-launcher-constellation-node");
        gtk_widget_set_tooltip_text(button, "Click to launch · Drag to move · Right-click to manage");
        g_object_set_data(G_OBJECT(button), "realmheart-constellation-node", node.get());

        GtkWidget* icon = make_launcher_icon(node->result.icon_name, 34);

        GtkWidget* label = gtk_label_new(node->result.title.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 13);
        gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(label, "realmheart-launcher-constellation-label");

        gtk_box_append(GTK_BOX(button), icon);
        gtk_box_append(GTK_BOX(button), label);

        GtkEventController* hover = gtk_event_controller_motion_new();
        g_signal_connect(hover, "enter", G_CALLBACK(+[](
            GtkEventController* controller, double, double, gpointer data
        ) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            GtkWidget* source = gtk_event_controller_get_widget(controller);
            auto* node = static_cast<ConstellationNode*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-constellation-node"
            ));
            if (node != nullptr) overlay->set_hovered_constellation_node(node);
        }), this);
        g_signal_connect(hover, "leave", G_CALLBACK(+[](
            GtkEventController* controller, gpointer data
        ) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            GtkWidget* source = gtk_event_controller_get_widget(controller);
            auto* node = static_cast<ConstellationNode*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-constellation-node"
            ));
            if (node != nullptr) overlay->set_hovered_constellation_node(nullptr);
        }), this);
        gtk_widget_add_controller(button, hover);

        // One primary gesture owns both click and drag. A release below
        // the drag threshold launches; a real drag moves and persists the node.
        GtkGesture* drag = gtk_gesture_drag_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(drag),
            GTK_PHASE_CAPTURE
        );
        g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](
            GtkGestureDrag* gesture, double start_x, double start_y, gpointer data
        ) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            GtkWidget* source = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
            auto* node = static_cast<ConstellationNode*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-constellation-node"
            ));
            if (node != nullptr) {
                overlay->begin_constellation_drag(
                    *node,
                    GTK_EVENT_CONTROLLER(gesture),
                    start_x,
                    start_y
                );
            }
        }), this);
        g_signal_connect(drag, "drag-update", G_CALLBACK(+[](
            GtkGestureDrag* gesture, double offset_x, double offset_y, gpointer data
        ) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            GtkWidget* source = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
            auto* node = static_cast<ConstellationNode*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-constellation-node"
            ));
            if (node != nullptr) {
                overlay->update_constellation_drag(
                    *node,
                    offset_x,
                    offset_y,
                    GTK_GESTURE(gesture)
                );
            }
        }), this);
        g_signal_connect(drag, "drag-end", G_CALLBACK(+[](
            GtkGestureDrag* gesture, double, double, gpointer data
        ) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            GtkWidget* source = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
            auto* node = static_cast<ConstellationNode*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-constellation-node"
            ));
            if (node == nullptr) return;

            const bool was_dragging = node->dragging;
            overlay->end_constellation_drag(*node);
            if (!was_dragging) overlay->activate_constellation_node(*node);
        }), this);
        gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(drag));

        GtkGesture* context = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(context), GDK_BUTTON_SECONDARY);
        g_signal_connect(context, "pressed", G_CALLBACK(+[](
            GtkGestureClick* gesture, int, double x, double y, gpointer data
        ) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            GtkWidget* source = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
            auto* node = static_cast<ConstellationNode*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-constellation-node"
            ));
            if (node != nullptr) overlay->show_constellation_menu(*node, x, y);
        }), this);
        gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(context));

        gtk_fixed_put(GTK_FIXED(constellation_canvas_), button, 0.0, 0.0);
        valid_layout.push_back(placement);
        constellation_nodes_.push_back(std::move(node));
    }

    if (valid_layout.size() != constellation_layout_.size()) {
        constellation_layout_ = std::move(valid_layout);
        save_constellation_layout();
    }

    layout_constellation();
}

std::pair<double, double> LauncherOverlay::constrain_constellation_position(
    double requested_x,
    double requested_y
) const {
    if (root_ == nullptr) return {requested_x, requested_y};

    const double width = std::max(1, gtk_widget_get_width(root_));
    const double height = std::max(1, gtk_widget_get_height(root_));
    const double maximum_x = std::max(
        kConstellationLeftInset,
        width - kConstellationRightInset - kConstellationNodeWidth
    );
    const double maximum_y = std::max(
        kConstellationTopInset,
        height - kConstellationBottomInset - kConstellationNodeHeight
    );

    const double x = std::clamp(
        requested_x,
        kConstellationLeftInset,
        maximum_x
    );
    double minimum_y = kConstellationTopInset;

    // The centre shell casts a broad lower shadow. Allowing a node to enter
    // that painted area makes the two surfaces visually splice together and
    // produces the broken/notched edge visible near the launcher. Keep a
    // dynamic gutter below the actual allocated shell only where their
    // horizontal spans intersect, so nodes can still sit higher at the sides.
    if (centre_shell_ != nullptr && constellation_canvas_ != nullptr) {
        graphene_rect_t centre_bounds{};
        if (gtk_widget_compute_bounds(
                centre_shell_,
                constellation_canvas_,
                &centre_bounds
            )) {
            const double keepout_left = centre_bounds.origin.x - kCentreKeepoutSide;
            const double keepout_right = centre_bounds.origin.x +
                centre_bounds.size.width + kCentreKeepoutSide;
            const bool horizontally_overlaps =
                x + kConstellationNodeWidth > keepout_left && x < keepout_right;
            if (horizontally_overlaps) {
                minimum_y = std::max(
                    minimum_y,
                    static_cast<double>(centre_bounds.origin.y + centre_bounds.size.height) +
                        kCentreKeepoutBottom
                );
            }
        }
    }

    minimum_y = std::min(minimum_y, maximum_y);
    const double y = std::clamp(requested_y, minimum_y, maximum_y);
    return {x, y};
}

void LauncherOverlay::layout_constellation() {
    if (constellation_canvas_ == nullptr || root_ == nullptr) return;

    const double width = std::max(1, gtk_widget_get_width(root_));
    const double height = std::max(1, gtk_widget_get_height(root_));
    const double usable_width = std::max(
        1.0,
        width - kConstellationLeftInset - kConstellationRightInset -
            kConstellationNodeWidth
    );
    const double usable_height = std::max(
        1.0,
        height - kConstellationTopInset - kConstellationBottomInset -
            kConstellationNodeHeight
    );

    bool needs_animation = false;
    for (const auto& node : constellation_nodes_) {
        const double requested_x =
            kConstellationLeftInset + node->normalized_x * usable_width;
        const double requested_y =
            kConstellationTopInset + node->normalized_y * usable_height;
        const auto [x, y] = constrain_constellation_position(requested_x, requested_y);
        node->current_x = x;
        node->current_y = y;

        if (!node->position_initialized) {
            const auto [initial_x, initial_y] = constellation_target_visible_
                ? std::pair{x, y}
                : constellation_emergence_position(*node);
            node->render_x = initial_x;
            node->render_y = initial_y;
            node->velocity_x = 0.0;
            node->velocity_y = 0.0;
            node->position_initialized = true;
            gtk_fixed_move(
                GTK_FIXED(constellation_canvas_),
                node->widget,
                initial_x,
                initial_y
            );
        } else if (std::hypot(node->render_x - x, node->render_y - y) >
                   kPositionEpsilon) {
            needs_animation = true;
        }
    }

    if (needs_animation) schedule_constellation_frame();
}

void LauncherOverlay::set_constellation_visible(bool visible) {
    if (constellation_canvas_ == nullptr ||
        constellation_target_visible_ == visible) {
        return;
    }

    constellation_target_visible_ = visible;
    if (visible) gtk_widget_set_visible(constellation_canvas_, TRUE);
    if (!visible) {
        hovered_constellation_node_ = nullptr;
        clear_constellation_selection();
    }

    const auto [search_x, search_y] = search_centre_in_constellation();
    double maximum_distance = 1.0;
    for (const auto& node : constellation_nodes_) {
        const double node_x = node->current_x +
            static_cast<double>(kConstellationNodeWidth) / 2.0;
        const double node_y = node->current_y +
            static_cast<double>(kConstellationNodeHeight) / 2.0;
        maximum_distance = std::max(
            maximum_distance,
            std::hypot(node_x - search_x, node_y - search_y)
        );
    }

    for (const auto& node : constellation_nodes_) {
        const double node_x = node->current_x +
            static_cast<double>(kConstellationNodeWidth) / 2.0;
        const double node_y = node->current_y +
            static_cast<double>(kConstellationNodeHeight) / 2.0;
        const double distance_ratio = std::clamp(
            std::hypot(node_x - search_x, node_y - search_y) / maximum_distance,
            0.0,
            1.0
        );

        // Reveal from the centre outward and retract from the outside inward.
        // Every node uses its own point along the instrument's lower edge, so
        // a rearranged constellation still fans naturally out from behind it.
        node->visibility_delay = visible
            ? distance_ratio * kVisibilityStagger
            : (1.0 - distance_ratio) * kVisibilityStagger;
        gtk_widget_set_can_target(node->widget, FALSE);
        if (!visible && node->dragging) {
            node->dragging = false;
            node->settling = true;
            gtk_widget_remove_css_class(node->widget, "dragging");
            gtk_widget_add_css_class(node->widget, "settling");
            gtk_widget_set_cursor_from_name(node->widget, "grab");
        }
    }

    schedule_constellation_frame();
}

LauncherOverlay::ConstellationNode* LauncherOverlay::active_constellation_highlight() const {
    // Selection remains visually owned by a node while it is dragged. The
    // reticle is attached to the node's rendered position in the frame loop,
    // so excluding dragging nodes here would leave the reticle behind and make
    // it jump to the drop point when the drag ends.
    if (hovered_constellation_node_ != nullptr) {
        return hovered_constellation_node_;
    }
    if (selected_constellation_node_ != nullptr) {
        return selected_constellation_node_;
    }
    return nullptr;
}

void LauncherOverlay::retarget_constellation_indicator(bool start_from_search) {
    ConstellationNode* target = active_constellation_highlight();
    selection_indicator_target_visible_ = target != nullptr;
    if (target == nullptr) {
        schedule_constellation_frame();
        return;
    }

    const double target_x = target->render_x -
        static_cast<double>(kSelectionIndicatorWidth - kConstellationNodeWidth) / 2.0;
    const double target_y = target->render_y -
        static_cast<double>(kSelectionIndicatorHeight - kConstellationNodeHeight) / 2.0;
    if (!selection_indicator_initialized_) {
        if (start_from_search) {
            const auto [search_x, search_y] = search_centre_in_constellation();
            selection_indicator_x_ = search_x -
                static_cast<double>(kSelectionIndicatorWidth) / 2.0;
            selection_indicator_y_ = search_y -
                static_cast<double>(kSelectionIndicatorHeight) / 2.0;
        } else {
            // Mouse hover has an obvious physical origin, so the first reticle
            // fades in around the hovered app instead of flying in from the
            // search box. Subsequent hover changes still glide spatially.
            selection_indicator_x_ = target_x;
            selection_indicator_y_ = target_y;
        }
        selection_indicator_velocity_x_ = 0.0;
        selection_indicator_velocity_y_ = 0.0;
        selection_indicator_initialized_ = true;
        if (selection_indicator_ != nullptr) {
            gtk_fixed_move(
                GTK_FIXED(constellation_canvas_),
                selection_indicator_,
                selection_indicator_x_,
                selection_indicator_y_
            );
        }
    } else {
        // Preserve travel direction and give the reticle enough momentum to
        // glide between both keyboard- and pointer-selected nodes.
        selection_indicator_velocity_x_ += std::clamp(
            (target_x - selection_indicator_x_) * kSelectionTravelImpulse,
            -820.0,
            820.0
        );
        selection_indicator_velocity_y_ += std::clamp(
            (target_y - selection_indicator_y_) * kSelectionTravelImpulse,
            -820.0,
            820.0
        );
    }

    schedule_constellation_frame();
}

void LauncherOverlay::clear_constellation_selection() {
    if (selected_constellation_node_ != nullptr) {
        gtk_widget_remove_css_class(
            selected_constellation_node_->widget,
            "keyboard-selected"
        );
    }
    selected_constellation_node_ = nullptr;
    retarget_constellation_indicator(false);
}

void LauncherOverlay::select_constellation_node(
    ConstellationNode* node,
    bool grab_focus
) {
    if (selected_constellation_node_ == node) {
        if (grab_focus && node != nullptr) gtk_widget_grab_focus(node->widget);
        return;
    }

    ConstellationNode* previous = selected_constellation_node_;
    if (previous != nullptr) {
        gtk_widget_remove_css_class(previous->widget, "keyboard-selected");
    }

    selected_constellation_node_ = node;
    if (node != nullptr) {
        gtk_widget_add_css_class(node->widget, "keyboard-selected");
        if (grab_focus) gtk_widget_grab_focus(node->widget);
    }

    retarget_constellation_indicator(true);
}

void LauncherOverlay::set_hovered_constellation_node(ConstellationNode* node) {
    if (hovered_constellation_node_ == node &&
        (node == nullptr || selected_constellation_node_ == node)) {
        return;
    }

    hovered_constellation_node_ = node;

    if (node != nullptr && !node->dragging &&
        selected_constellation_node_ != node) {
        // Pointer navigation becomes the persistent spatial selection instead
        // of a temporary visual override. This prevents the previous keyboard
        // selection from reclaiming the reticle when the hovered node's lift
        // animation briefly moves its GTK hit surface away from the pointer.
        // Do not grab widget focus: the search entry must remain ready for text.
        if (selected_constellation_node_ != nullptr) {
            gtk_widget_remove_css_class(
                selected_constellation_node_->widget,
                "keyboard-selected"
            );
        }
        selected_constellation_node_ = node;
        gtk_widget_add_css_class(node->widget, "keyboard-selected");
    }

    retarget_constellation_indicator(false);
}

bool LauncherOverlay::navigate_constellation(SpatialDirection direction) {
    if (constellation_nodes_.empty() ||
        constellation_canvas_ == nullptr ||
        !gtk_widget_get_visible(constellation_canvas_)) {
        return false;
    }

    // Last input modality wins: an arrow press takes visual ownership away
    // from a stationary pointer until it enters a node again.
    set_hovered_constellation_node(nullptr);

    double origin_x = 0.0;
    double origin_y = 0.0;
    if (selected_constellation_node_ != nullptr) {
        origin_x = selected_constellation_node_->current_x +
            static_cast<double>(kConstellationNodeWidth) / 2.0;
        origin_y = selected_constellation_node_->current_y +
            static_cast<double>(kConstellationNodeHeight) / 2.0;
    } else {
        const graphene_point_t search_centre{
            static_cast<float>(gtk_widget_get_width(search_entry_)) / 2.0F,
            static_cast<float>(gtk_widget_get_height(search_entry_)) / 2.0F,
        };
        graphene_point_t window_centre{};
        graphene_point_t canvas_centre{};
        if (gtk_widget_compute_point(
                search_entry_,
                GTK_WIDGET(window_),
                &search_centre,
                &window_centre
            ) &&
            gtk_widget_compute_point(
                GTK_WIDGET(window_),
                constellation_canvas_,
                &window_centre,
                &canvas_centre
            )) {
            origin_x = canvas_centre.x;
            origin_y = canvas_centre.y;
        } else {
            origin_x = static_cast<double>(gtk_widget_get_width(root_)) / 2.0;
            origin_y = static_cast<double>(kConstellationTopInset);
        }
    }

    double direction_x = 0.0;
    double direction_y = 0.0;
    switch (direction) {
    case SpatialDirection::Left:
        direction_x = -1.0;
        break;
    case SpatialDirection::Right:
        direction_x = 1.0;
        break;
    case SpatialDirection::Up:
        direction_y = -1.0;
        break;
    case SpatialDirection::Down:
        direction_y = 1.0;
        break;
    }

    ConstellationNode* best = nullptr;
    double best_score = std::numeric_limits<double>::infinity();
    for (const auto& candidate_ptr : constellation_nodes_) {
        ConstellationNode* candidate = candidate_ptr.get();
        if (candidate == selected_constellation_node_) continue;

        const double candidate_x = candidate->current_x +
            static_cast<double>(kConstellationNodeWidth) / 2.0;
        const double candidate_y = candidate->current_y +
            static_cast<double>(kConstellationNodeHeight) / 2.0;
        const double delta_x = candidate_x - origin_x;
        const double delta_y = candidate_y - origin_y;
        const double forward = delta_x * direction_x + delta_y * direction_y;
        if (forward <= 1.0) continue;

        const double perpendicular = std::abs(
            delta_x * direction_y - delta_y * direction_x
        );
        const double angular_penalty = perpendicular / std::max(1.0, forward);
        const double score = forward + perpendicular * 1.85 + angular_penalty * 120.0;
        if (score < best_score) {
            best_score = score;
            best = candidate;
        }
    }

    if (best != nullptr) select_constellation_node(best, true);
    return true;
}

bool LauncherOverlay::pointer_position_in_constellation(
    GtkEventController* controller,
    double& x,
    double& y
) const {
    if (controller == nullptr || constellation_canvas_ == nullptr || window_ == nullptr) {
        return false;
    }

    GdkEvent* event = gtk_event_controller_get_current_event(controller);
    if (event == nullptr) return false;

    double surface_x = 0.0;
    double surface_y = 0.0;
    if (!gdk_event_get_position(event, &surface_x, &surface_y)) return false;

    const graphene_point_t source{
        static_cast<float>(surface_x),
        static_cast<float>(surface_y),
    };
    graphene_point_t target{};
    if (!gtk_widget_compute_point(
            GTK_WIDGET(window_),
            constellation_canvas_,
            &source,
            &target
        )) {
        return false;
    }

    x = target.x;
    y = target.y;
    return true;
}

std::pair<double, double> LauncherOverlay::search_centre_in_constellation() const {
    if (search_entry_ == nullptr || constellation_canvas_ == nullptr ||
        root_ == nullptr) {
        return {0.0, 0.0};
    }

    const graphene_point_t search_centre{
        static_cast<float>(gtk_widget_get_width(search_entry_)) / 2.0F,
        static_cast<float>(gtk_widget_get_height(search_entry_)) / 2.0F,
    };
    graphene_point_t window_centre{};
    graphene_point_t canvas_centre{};
    if (window_ != nullptr &&
        gtk_widget_compute_point(
            search_entry_,
            GTK_WIDGET(window_),
            &search_centre,
            &window_centre
        ) &&
        gtk_widget_compute_point(
            GTK_WIDGET(window_),
            constellation_canvas_,
            &window_centre,
            &canvas_centre
        )) {
        return {canvas_centre.x, canvas_centre.y};
    }

    return {
        static_cast<double>(gtk_widget_get_width(root_)) / 2.0,
        static_cast<double>(kConstellationTopInset),
    };
}


std::pair<double, double> LauncherOverlay::constellation_emergence_position(
    const ConstellationNode& node
) const {
    // Derive the emergence edge from the same animation constants that drive
    // the central instrument. Querying GTK bounds here is unsafe while the
    // overlay is being mapped: GTK can temporarily report a successful zero-
    // width allocation, which makes the clamp range inverted and aborts the
    // shell on builds with libstdc++ assertions enabled.
    const double root_width = root_ != nullptr
        ? static_cast<double>(gtk_widget_get_width(root_))
        : 0.0;
    const double frame = ease_out_cubic(interval_progress(
        central_progress_,
        0.0,
        0.72
    ));
    const double centre_width = interpolate(
        static_cast<double>(kCentreStartWidth),
        static_cast<double>(kCentreFinalWidth),
        frame
    );
    const double centre_top = interpolate(
        static_cast<double>(kCentreStartTopMargin),
        static_cast<double>(kCentreFinalTopMargin),
        frame
    );
    const double centre_left = (std::max(root_width, centre_width) - centre_width) /
        2.0;
    const double centre_midpoint = centre_left + centre_width / 2.0;
    const double final_node_centre = node.current_x +
        static_cast<double>(kConstellationNodeWidth) / 2.0;
    const double compressed_centre = centre_midpoint +
        (final_node_centre - centre_midpoint) * kEmergenceHorizontalCompression;

    // Keep the legal range valid even if the centre is ever made narrower in
    // a future animation pass. std::clamp requires lower <= upper.
    const double maximum_inset = std::max(0.0, centre_width / 2.0 - 1.0);
    const double safe_inset = std::min(kEmergenceEdgeInset, maximum_inset);
    const double lower_bound = centre_left + safe_inset;
    const double upper_bound = centre_left + centre_width - safe_inset;
    const double emergence_centre = lower_bound <= upper_bound
        ? std::clamp(compressed_centre, lower_bound, upper_bound)
        : centre_midpoint;

    return {
        emergence_centre - static_cast<double>(kConstellationNodeWidth) / 2.0,
        centre_top + static_cast<double>(kCentreHeight) -
            static_cast<double>(kConstellationNodeHeight) + kEmergencePeek,
    };
}

void LauncherOverlay::schedule_constellation_frame() {
    if (constellation_tick_id_ != 0 || root_ == nullptr) return;
    constellation_last_frame_time_ = 0;
    constellation_tick_id_ = gtk_widget_add_tick_callback(
        root_,
        +[](GtkWidget*, GdkFrameClock* frame_clock, gpointer data) -> gboolean {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            if (overlay->advance_constellation_frame(frame_clock)) {
                return G_SOURCE_CONTINUE;
            }
            overlay->constellation_tick_id_ = 0;
            overlay->constellation_last_frame_time_ = 0;
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

bool LauncherOverlay::advance_constellation_frame(GdkFrameClock* frame_clock) {
    if (constellation_canvas_ == nullptr) return false;

    const gint64 frame_time = gdk_frame_clock_get_frame_time(frame_clock);
    double elapsed = 1.0 / 60.0;
    if (constellation_last_frame_time_ != 0) {
        elapsed = static_cast<double>(frame_time - constellation_last_frame_time_) /
            1'000'000.0;
    }
    constellation_last_frame_time_ = frame_time;
    elapsed = std::clamp(elapsed, 1.0 / 240.0, 0.05);

    bool animation_active = false;
    bool every_node_hidden = !constellation_target_visible_;
    ConstellationNode* highlight_node = active_constellation_highlight();

    for (const auto& node : constellation_nodes_) {
        double animation_elapsed = elapsed;
        if (node->visibility_delay > 0.0) {
            node->visibility_delay -= elapsed;
            if (node->visibility_delay > 0.0) {
                animation_elapsed = 0.0;
                animation_active = true;
            } else {
                animation_elapsed = -node->visibility_delay;
                node->visibility_delay = 0.0;
            }
        }

        const double selection_target =
            node.get() == highlight_node &&
            constellation_target_visible_ ? 1.0 : 0.0;
        if (animation_elapsed > 0.0) {
            const int selection_steps = std::max(
                1,
                static_cast<int>(std::ceil(animation_elapsed / (1.0 / 120.0)))
            );
            const double selection_step = animation_elapsed /
                static_cast<double>(selection_steps);
            for (int index = 0; index < selection_steps; ++index) {
                const double acceleration =
                    (selection_target - node->selection_amount) *
                        kSelectionNodeSpring -
                    node->selection_velocity * kSelectionNodeDamping;
                node->selection_velocity += acceleration * selection_step;
                node->selection_amount +=
                    node->selection_velocity * selection_step;
            }
            node->selection_amount = std::clamp(
                node->selection_amount,
                -0.08,
                1.08
            );
        }
        if (std::abs(node->selection_amount - selection_target) <= 0.002 &&
            std::abs(node->selection_velocity) <= 0.02) {
            node->selection_amount = selection_target;
            node->selection_velocity = 0.0;
        } else {
            animation_active = true;
        }

        const double target_opacity = constellation_target_visible_ ? 1.0 : 0.0;
        if (animation_elapsed > 0.0 &&
            std::abs(node->opacity - target_opacity) > 0.001) {
            const double opacity_blend = 1.0 -
                std::exp(-kOpacityResponse * animation_elapsed);
            node->opacity += (target_opacity - node->opacity) * opacity_blend;
        }
        if (std::abs(node->opacity - target_opacity) <= 0.002) {
            node->opacity = target_opacity;
        } else {
            animation_active = true;
        }
        // Keep nodes opaque for most of the return journey so they visibly
        // disappear behind the central instrument instead of merely fading in
        // place. On reveal, they become visible as soon as they clear its edge.
        const double painted_opacity = smooth_step(
            interval_progress(node->opacity, 0.06, 0.48)
        );
        gtk_widget_set_opacity(node->widget, painted_opacity);

        const auto [emergence_x, emergence_y] =
            constellation_emergence_position(*node);
        const double visible_amount = smooth_step(node->opacity);
        const double trajectory_arc = std::sin(
            visible_amount * std::numbers::pi
        ) * kEmergenceArc;
        const double target_x = interpolate(
            emergence_x,
            node->current_x,
            visible_amount
        );
        const double target_y = interpolate(
            emergence_y,
            node->current_y,
            visible_amount
        ) + trajectory_arc -
            (node->dragging ? kDragLift : 0.0) -
            node->selection_amount * kSelectionLift;

        const bool visibility_motion = node->visibility_delay > 0.0 ||
            node->opacity < 0.998 || !constellation_target_visible_;
        const double spring = node->dragging
            ? kDragSpring
            : (node->settling
                ? kSettleSpring
                : (visibility_motion ? kVisibilitySpring : kIdleSpring));
        const double damping = node->dragging
            ? kDragDamping
            : (node->settling
                ? kSettleDamping
                : (visibility_motion ? kVisibilityDamping : kIdleDamping));

        // Split long compositor frames into small integration steps. This keeps
        // the spring stable through an occasional frame hitch without losing
        // the soft, slightly elastic follow while dragging.
        const int step_count = std::max(
            1,
            static_cast<int>(std::ceil(elapsed / (1.0 / 120.0)))
        );
        const double step = elapsed / static_cast<double>(step_count);
        for (int index = 0; index < step_count; ++index) {
            const double acceleration_x =
                (target_x - node->render_x) * spring - node->velocity_x * damping;
            const double acceleration_y =
                (target_y - node->render_y) * spring - node->velocity_y * damping;
            node->velocity_x = std::clamp(
                node->velocity_x + acceleration_x * step,
                -2400.0,
                2400.0
            );
            node->velocity_y = std::clamp(
                node->velocity_y + acceleration_y * step,
                -2400.0,
                2400.0
            );
            node->render_x += node->velocity_x * step;
            node->render_y += node->velocity_y * step;
        }

        const double position_error = std::hypot(
            target_x - node->render_x,
            target_y - node->render_y
        );
        const double speed = std::hypot(node->velocity_x, node->velocity_y);
        if (!node->dragging && position_error <= kPositionEpsilon &&
            speed <= kVelocityEpsilon) {
            node->render_x = target_x;
            node->render_y = target_y;
            node->velocity_x = 0.0;
            node->velocity_y = 0.0;
            if (node->settling) {
                node->settling = false;
                gtk_widget_remove_css_class(node->widget, "settling");
            }
        } else {
            animation_active = true;
        }

        gtk_fixed_move(
            GTK_FIXED(constellation_canvas_),
            node->widget,
            node->render_x,
            node->render_y
        );

        if (constellation_target_visible_) {
            const bool interactive = node->opacity >= 0.62;
            gtk_widget_set_can_target(node->widget, interactive);
            every_node_hidden = false;
        } else {
            gtk_widget_set_can_target(node->widget, FALSE);
            if (node->opacity > 0.002) every_node_hidden = false;
        }
    }

    if (selection_indicator_ != nullptr) {
        const bool indicator_visible = selection_indicator_target_visible_ &&
            highlight_node != nullptr &&
            constellation_target_visible_;
        const double opacity_target = indicator_visible ? 1.0 : 0.0;
        const double opacity_blend = 1.0 -
            std::exp(-kSelectionIndicatorOpacityResponse * elapsed);
        selection_indicator_opacity_ +=
            (opacity_target - selection_indicator_opacity_) * opacity_blend;
        if (std::abs(selection_indicator_opacity_ - opacity_target) <= 0.002) {
            selection_indicator_opacity_ = opacity_target;
        } else {
            animation_active = true;
        }

        if (indicator_visible) {
            const double target_x = highlight_node->render_x -
                static_cast<double>(
                    kSelectionIndicatorWidth - kConstellationNodeWidth
                ) / 2.0;
            const double target_y = highlight_node->render_y -
                static_cast<double>(
                    kSelectionIndicatorHeight - kConstellationNodeHeight
                ) / 2.0;
            if (!selection_indicator_initialized_) {
                selection_indicator_x_ = target_x;
                selection_indicator_y_ = target_y;
                selection_indicator_initialized_ = true;
            }

            if (highlight_node->dragging || highlight_node->settling) {
                // During direct manipulation the reticle is part of the app,
                // not an independent cursor. Lock it to the node's rendered
                // position so long drags and the landing wobble cannot expose
                // a trailing border or a final-position teleport.
                selection_indicator_x_ = target_x;
                selection_indicator_y_ = target_y;
                selection_indicator_velocity_x_ = 0.0;
                selection_indicator_velocity_y_ = 0.0;
            } else {
                const int step_count = std::max(
                    1,
                    static_cast<int>(std::ceil(elapsed / (1.0 / 120.0)))
                );
                const double step = elapsed / static_cast<double>(step_count);
                for (int index = 0; index < step_count; ++index) {
                    const double acceleration_x =
                        (target_x - selection_indicator_x_) *
                            kSelectionIndicatorSpring -
                        selection_indicator_velocity_x_ *
                            kSelectionIndicatorDamping;
                    const double acceleration_y =
                        (target_y - selection_indicator_y_) *
                            kSelectionIndicatorSpring -
                        selection_indicator_velocity_y_ *
                            kSelectionIndicatorDamping;
                    selection_indicator_velocity_x_ = std::clamp(
                        selection_indicator_velocity_x_ + acceleration_x * step,
                        -2800.0,
                        2800.0
                    );
                    selection_indicator_velocity_y_ = std::clamp(
                        selection_indicator_velocity_y_ + acceleration_y * step,
                        -2800.0,
                        2800.0
                    );
                    selection_indicator_x_ +=
                        selection_indicator_velocity_x_ * step;
                    selection_indicator_y_ +=
                        selection_indicator_velocity_y_ * step;
                }

                const double indicator_error = std::hypot(
                    target_x - selection_indicator_x_,
                    target_y - selection_indicator_y_
                );
                const double indicator_speed = std::hypot(
                    selection_indicator_velocity_x_,
                    selection_indicator_velocity_y_
                );
                if (indicator_error <= 0.08 && indicator_speed <= 2.0) {
                    selection_indicator_x_ = target_x;
                    selection_indicator_y_ = target_y;
                    selection_indicator_velocity_x_ = 0.0;
                    selection_indicator_velocity_y_ = 0.0;
                } else {
                    animation_active = true;
                }
            }
        }

        gtk_widget_set_opacity(
            selection_indicator_,
            smooth_step(selection_indicator_opacity_)
        );
        if (selection_indicator_initialized_) {
            gtk_fixed_move(
                GTK_FIXED(constellation_canvas_),
                selection_indicator_,
                selection_indicator_x_,
                selection_indicator_y_
            );
        }
    }

    if (every_node_hidden && constellation_canvas_ != nullptr) {
        gtk_widget_set_visible(constellation_canvas_, FALSE);
    }
    return animation_active;
}

bool LauncherOverlay::point_hits_constellation_node(double x, double y) const {
    return std::any_of(
        constellation_nodes_.begin(),
        constellation_nodes_.end(),
        [x, y](const std::unique_ptr<ConstellationNode>& node) {
            return node->opacity > 0.20 &&
                   x >= node->render_x &&
                   x <= node->render_x + kConstellationNodeWidth &&
                   y >= node->render_y &&
                   y <= node->render_y + kConstellationNodeHeight;
        }
    );
}

bool LauncherOverlay::constellation_contains(std::string_view application_id) const {
    return std::any_of(
        constellation_layout_.begin(),
        constellation_layout_.end(),
        [application_id](const ConstellationPlacement& placement) {
            return placement.application_id == application_id;
        }
    );
}

void LauncherOverlay::pin_constellation_application(std::string_view application_id) {
    if (application_id.empty() || constellation_contains(application_id) ||
        constellation_layout_.size() >= kMaximumConstellationApplications) {
        return;
    }
    if (!service_.application_by_id(application_id).has_value()) return;

    const auto [x, y] = default_constellation_position(constellation_layout_.size());
    constellation_layout_.push_back({std::string(application_id), x, y});
    constellation_layout_loaded_ = true;
    save_constellation_layout();
    rebuild_constellation();
}

void LauncherOverlay::unpin_constellation_application(std::string_view application_id) {
    const auto previous_size = constellation_layout_.size();
    std::erase_if(constellation_layout_, [application_id](const ConstellationPlacement& placement) {
        return placement.application_id == application_id;
    });
    if (constellation_layout_.size() == previous_size) return;

    constellation_layout_loaded_ = true;
    save_constellation_layout();
    rebuild_constellation();
}

void LauncherOverlay::toggle_constellation_application(std::string_view application_id) {
    if (constellation_contains(application_id)) {
        unpin_constellation_application(application_id);
    } else {
        pin_constellation_application(application_id);
    }
}

void LauncherOverlay::activate_constellation_node(ConstellationNode& node) {
    if ((node.result.kind == services::LauncherResultKind::Command ||
         node.result.kind == services::LauncherResultKind::Action) &&
        command_receipts_.execute(node.result)) {
        return;
    }
    if (service_.activate(node.result)) hide();
}

void LauncherOverlay::begin_constellation_drag(
    ConstellationNode& node,
    GtkEventController* controller,
    double grab_x,
    double grab_y
) {
    if (hovered_constellation_node_ == &node) {
        set_hovered_constellation_node(nullptr);
    }
    select_constellation_node(&node, true);
    node.drag_grab_x = grab_x;
    node.drag_grab_y = grab_y;
    node.drag_pointer_start_x = node.current_x + grab_x;
    node.drag_pointer_start_y = node.current_y + grab_y;
    if (!pointer_position_in_constellation(
            controller,
            node.drag_pointer_start_x,
            node.drag_pointer_start_y
        )) {
        // Fall back to the node-local press coordinates when GTK cannot
        // translate the current event into constellation-space coordinates.
        node.drag_pointer_start_x = node.current_x + grab_x;
        node.drag_pointer_start_y = node.current_y + grab_y;
    }
    node.dragging = false;
    node.settling = false;
    gtk_widget_remove_css_class(node.widget, "settling");
}

void LauncherOverlay::update_constellation_drag(
    ConstellationNode& node,
    double offset_x,
    double offset_y,
    GtkGesture* gesture
) {
    double pointer_x = node.drag_pointer_start_x + offset_x;
    double pointer_y = node.drag_pointer_start_y + offset_y;
    if (!pointer_position_in_constellation(
            GTK_EVENT_CONTROLLER(gesture),
            pointer_x,
            pointer_y
        )) {
        // GtkGestureDrag offsets remain a stable fallback if the current
        // event position is temporarily unavailable during a frame.
        pointer_x = node.drag_pointer_start_x + offset_x;
        pointer_y = node.drag_pointer_start_y + offset_y;
    }

    const double pointer_distance = std::hypot(
        pointer_x - node.drag_pointer_start_x,
        pointer_y - node.drag_pointer_start_y
    );
    if (!node.dragging && pointer_distance >= kDragThreshold) {
        node.dragging = true;
        node.settling = false;
        gtk_widget_remove_css_class(node.widget, "settling");
        gtk_widget_add_css_class(node.widget, "dragging");
        gtk_widget_set_cursor_from_name(node.widget, "grabbing");
        gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
        schedule_constellation_frame();
    }
    if (!node.dragging) return;

    const double width = std::max(1, gtk_widget_get_width(root_));
    const double height = std::max(1, gtk_widget_get_height(root_));
    const double maximum_x = std::max(
        kConstellationLeftInset,
        width - kConstellationRightInset - kConstellationNodeWidth
    );
    const double maximum_y = std::max(
        kConstellationTopInset,
        height - kConstellationBottomInset - kConstellationNodeHeight
    );

    const auto [x, y] = constrain_constellation_position(
        pointer_x - node.drag_grab_x,
        pointer_y - node.drag_grab_y
    );
    node.current_x = x;
    node.current_y = y;
    schedule_constellation_frame();

    const double usable_width = std::max(1.0, maximum_x - kConstellationLeftInset);
    const double usable_height = std::max(1.0, maximum_y - kConstellationTopInset);
    node.normalized_x = std::clamp((x - kConstellationLeftInset) / usable_width, 0.0, 1.0);
    node.normalized_y = std::clamp((y - kConstellationTopInset) / usable_height, 0.0, 1.0);
}

void LauncherOverlay::end_constellation_drag(ConstellationNode& node) {
    gtk_widget_remove_css_class(node.widget, "dragging");
    gtk_widget_set_cursor_from_name(node.widget, "grab");
    if (!node.dragging) return;

    node.dragging = false;
    node.settling = true;
    gtk_widget_add_css_class(node.widget, "settling");

    // The node was visually lifted while held. A small downward release impulse
    // turns the spring return into a short landing wobble instead of a sterile
    // snap, while preserving the exact persisted drop coordinate.
    const double grip_bias = node.drag_grab_x <
        static_cast<double>(kConstellationNodeWidth) / 2.0 ? 1.0 : -1.0;
    node.velocity_x = node.velocity_x * 1.04 + grip_bias * 42.0;
    node.velocity_y += 82.0;
    schedule_constellation_frame();

    const auto iterator = std::find_if(
        constellation_layout_.begin(),
        constellation_layout_.end(),
        [&node](const ConstellationPlacement& placement) {
            return placement.application_id == node.result.id;
        }
    );
    if (iterator != constellation_layout_.end()) {
        iterator->normalized_x = node.normalized_x;
        iterator->normalized_y = node.normalized_y;
        save_constellation_layout();
    }
}

void LauncherOverlay::show_constellation_menu(ConstellationNode& node, double x, double y) {
    if (node.menu == nullptr) {
        node.menu = gtk_popover_new();
        gtk_popover_set_autohide(GTK_POPOVER(node.menu), TRUE);
        gtk_widget_add_css_class(node.menu, "realmheart-launcher-constellation-menu");
        gtk_widget_set_parent(node.menu, node.widget);

        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(box, 6);
        gtk_widget_set_margin_end(box, 6);
        gtk_widget_set_margin_top(box, 6);
        gtk_widget_set_margin_bottom(box, 6);

        GtkWidget* unpin = gtk_button_new_with_label("Unpin from launcher");
        gtk_button_set_has_frame(GTK_BUTTON(unpin), FALSE);
        gtk_widget_add_css_class(unpin, "realmheart-launcher-constellation-menu-item");
        g_object_set_data(G_OBJECT(unpin), "realmheart-constellation-node", &node);
        g_signal_connect(unpin, "clicked", G_CALLBACK(+[](GtkButton* source, gpointer data) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            auto* target = static_cast<ConstellationNode*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-constellation-node"
            ));
            if (target != nullptr) {
                const std::string application_id = target->result.id;
                overlay->unpin_constellation_application(application_id);
            }
        }), this);
        gtk_box_append(GTK_BOX(box), unpin);
        gtk_popover_set_child(GTK_POPOVER(node.menu), box);
    }

    const GdkRectangle pointing_to{
        static_cast<int>(x),
        static_cast<int>(y),
        1,
        1,
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(node.menu), &pointing_to);
    gtk_popover_popup(GTK_POPOVER(node.menu));
}

bool LauncherOverlay::parse_clipboard_query(
    std::string_view query,
    std::string& filter
) const {
    const std::size_t first = query.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return false;
    query.remove_prefix(first);

    constexpr std::string_view prefix = ">clip";
    if (!query.starts_with(prefix)) return false;
    if (query.size() > prefix.size() &&
        std::isspace(static_cast<unsigned char>(query[prefix.size()])) == 0) {
        return false;
    }

    query.remove_prefix(prefix.size());
    const std::size_t filter_start = query.find_first_not_of(" \t\n\r");
    if (filter_start == std::string_view::npos) {
        filter.clear();
        return true;
    }
    query.remove_prefix(filter_start);
    const std::size_t filter_end = query.find_last_not_of(" \t\n\r");
    filter = std::string(query.substr(0, filter_end + 1));
    return true;
}

bool LauncherOverlay::parse_clipboard_clear_query(std::string_view query) const {
    const std::size_t first = query.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return false;
    query.remove_prefix(first);
    const std::size_t last = query.find_last_not_of(" \t\n\r");
    query = query.substr(0, last + 1);
    return query == ">clear";
}

bool LauncherOverlay::parse_emoji_query(
    std::string_view query,
    std::string& filter
) const {
    const std::size_t first = query.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return false;
    query.remove_prefix(first);

    constexpr std::string_view prefix = ">emoji";
    if (!query.starts_with(prefix)) return false;
    if (query.size() > prefix.size() &&
        std::isspace(static_cast<unsigned char>(query[prefix.size()])) == 0) {
        return false;
    }

    query.remove_prefix(prefix.size());
    const std::size_t filter_start = query.find_first_not_of(" \t\n\r");
    if (filter_start == std::string_view::npos) {
        filter.clear();
        return true;
    }
    query.remove_prefix(filter_start);
    const std::size_t filter_end = query.find_last_not_of(" \t\n\r");
    filter = std::string(query.substr(0, filter_end + 1));
    return true;
}

std::string LauncherOverlay::empty_results_message() const {
    if (search_mode_ == SearchMode::ClipboardClear &&
        !clipboard_status_message_.empty()) {
        return clipboard_status_message_;
    }
    if (search_mode_ != SearchMode::Clipboard) {
        if (search_mode_ != SearchMode::Emoji) {
            return "No matching application or action";
        }
        if (!emoji_status_message_.empty()) return emoji_status_message_;
        if (emoji_loading_ && !emoji_database_loaded_) {
            return "Loading emoji index…";
        }
        if (!emoji_database_loaded_) return "Emoji index is unavailable";
        if (emoji_database_text_.empty()) return "Emoji index is empty";
        return "No matching emoji";
    }
    if (!clipboard_status_message_.empty()) return clipboard_status_message_;
    if (clipboard_loading_ && !clipboard_history_loaded_) {
        return "Loading clipboard history…";
    }
    if (!clipboard_history_loaded_) return "Clipboard history is unavailable";
    if (clipboard_history_output_.empty()) return "Clipboard history is empty";
    return "No matching clipboard entry";
}

void LauncherOverlay::clear_clipboard_thumbnail_cache() {
    for (auto& [id, thumbnail] : clipboard_thumbnail_cache_) {
        static_cast<void>(id);
        if (thumbnail.texture != nullptr) g_object_unref(thumbnail.texture);
    }
    clipboard_thumbnail_cache_.clear();
    clipboard_thumbnail_lru_.clear();
    clipboard_rows_.clear();
}

void LauncherOverlay::leave_clipboard_mode() {
    if (search_mode_ != SearchMode::Clipboard &&
        search_mode_ != SearchMode::ClipboardClear) {
        return;
    }

    search_mode_ = SearchMode::Normal;
    clipboard_async_state_->generation.fetch_add(1, std::memory_order_acq_rel);
    clipboard_filter_.clear();
    clipboard_status_message_.clear();
    clipboard_all_results_.clear();
    clipboard_rendered_count_ = 0;
    clipboard_loading_ = false;
    clipboard_clear_armed_ = false;
    if (clipboard_page_growth_idle_id_ != 0) {
        g_source_remove(clipboard_page_growth_idle_id_);
        clipboard_page_growth_idle_id_ = 0;
    }
    clear_clipboard_thumbnail_cache();
    gtk_revealer_set_transition_duration(GTK_REVEALER(results_revealer_), 170);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(results_scroller_),
        kNormalResultsMaximumHeight
    );
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search_entry_),
        "Search apps, calculate, or run a command"
    );
}

void LauncherOverlay::enter_clipboard_mode(std::string filter) {
    leave_emoji_mode();
    const bool mode_changed = search_mode_ != SearchMode::Clipboard;
    if (mode_changed) leave_clipboard_mode();

    const bool filter_changed = filter != clipboard_filter_;
    search_mode_ = SearchMode::Clipboard;
    clipboard_filter_ = std::move(filter);
    clipboard_status_message_.clear();
    clipboard_clear_armed_ = false;
    gtk_revealer_set_transition_duration(GTK_REVEALER(results_revealer_), 220);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(results_scroller_),
        kClipboardResultsMaximumHeight
    );
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search_entry_),
        "Clipboard › Search copied items"
    );

    if (filter_changed) {
        clipboard_all_results_.clear();
        clipboard_rendered_count_ = 0;
    }

    // Reuse the last lightweight cliphist listing immediately. SUPER+V now
    // feels instant after the first visit, while a background refresh still
    // picks up anything copied since the previous opening.
    if (clipboard_history_loaded_) {
        rebuild_clipboard_results();
    } else if (!clipboard_loading_) {
        clipboard_status_message_ = "Loading clipboard history…";
        current_results_.clear();
        rebuild_results();
    }

    if (mode_changed && !clipboard_loading_) request_clipboard_history();
}

void LauncherOverlay::enter_clipboard_clear_mode() {
    leave_emoji_mode();
    const bool mode_changed = search_mode_ != SearchMode::ClipboardClear;
    if (mode_changed) leave_clipboard_mode();

    search_mode_ = SearchMode::ClipboardClear;
    clipboard_status_message_.clear();
    if (mode_changed) clipboard_clear_armed_ = false;
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(results_scroller_),
        kNormalResultsMaximumHeight
    );
    current_results_ = {
        services::launcher_clipboard_clear_result(clipboard_clear_armed_)
    };
    rebuild_results();
}

void LauncherOverlay::leave_emoji_mode() {
    if (search_mode_ != SearchMode::Emoji) return;

    search_mode_ = SearchMode::Normal;
    emoji_async_state_->generation.fetch_add(1, std::memory_order_acq_rel);
    emoji_filter_.clear();
    emoji_status_message_.clear();
    emoji_all_results_.clear();
    emoji_rendered_count_ = 0;
    emoji_loading_ = false;
    if (emoji_page_growth_idle_id_ != 0) {
        g_source_remove(emoji_page_growth_idle_id_);
        emoji_page_growth_idle_id_ = 0;
    }
    gtk_revealer_set_transition_duration(GTK_REVEALER(results_revealer_), 170);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(results_scroller_),
        kNormalResultsMaximumHeight
    );
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search_entry_),
        "Search apps, calculate, or run a command"
    );
}

void LauncherOverlay::enter_emoji_mode(std::string filter) {
    leave_clipboard_mode();
    const bool mode_changed = search_mode_ != SearchMode::Emoji;

    const bool filter_changed = filter != emoji_filter_;
    search_mode_ = SearchMode::Emoji;
    emoji_filter_ = std::move(filter);
    emoji_status_message_.clear();
    gtk_revealer_set_transition_duration(GTK_REVEALER(results_revealer_), 220);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(results_scroller_),
        kEmojiResultsMaximumHeight
    );
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(search_entry_),
        "Emoji › Search by name or keyword"
    );

    if (filter_changed) {
        emoji_all_results_.clear();
        emoji_rendered_count_ = 0;
    }

    if (emoji_database_loaded_) {
        rebuild_emoji_results();
    } else if (!emoji_loading_) {
        emoji_status_message_ = "Loading emoji index…";
        current_results_.clear();
        rebuild_results();
    }

    if (mode_changed && !emoji_loading_ && !emoji_database_loaded_) {
        request_emoji_database();
    }
}

void LauncherOverlay::request_emoji_database() {
    auto state = emoji_async_state_;
    const std::uint64_t generation =
        state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;

    emoji_loading_ = true;
    emoji_status_message_ = "Loading emoji index…";
    current_results_.clear();
    rebuild_results();

    struct Completion {
        std::shared_ptr<EmojiAsyncState> state;
        std::uint64_t generation = 0;
        bool succeeded = false;
        std::string contents;
    };

    const auto callback = +[](gpointer data) -> gboolean {
        std::unique_ptr<Completion> completion(static_cast<Completion*>(data));
        LauncherOverlay* owner = completion->state->owner.load(
            std::memory_order_acquire
        );
        if (owner == nullptr ||
            completion->generation != completion->state->generation.load(
                std::memory_order_acquire
            ) || owner->search_mode_ != SearchMode::Emoji) {
            return G_SOURCE_REMOVE;
        }

        owner->emoji_loading_ = false;
        if (!completion->succeeded) {
            owner->emoji_database_loaded_ = false;
            owner->emoji_database_text_.clear();
            owner->emoji_status_message_ = "Unable to read fuzzel emoji data";
            owner->current_results_.clear();
            owner->rebuild_results();
            return G_SOURCE_REMOVE;
        }

        owner->emoji_database_text_ = std::move(completion->contents);
        owner->emoji_database_loaded_ = true;
        owner->emoji_status_message_.clear();
        owner->rebuild_emoji_results();
        return G_SOURCE_REMOVE;
    };

    const bool posted = realmheart::core::shared_task_executor().post(
        [state, generation, callback] {
            constexpr std::uintmax_t maximum_size = 2U * 1024U * 1024U;
            const fs::path path = emoji_script_path();
            std::error_code error;
            const std::uintmax_t size = fs::file_size(path, error);
            bool succeeded = !error && size <= maximum_size;
            std::string contents;

            if (succeeded) {
                std::ifstream stream(path, std::ios::binary);
                if (stream) {
                    contents.assign(
                        std::istreambuf_iterator<char>(stream),
                        std::istreambuf_iterator<char>()
                    );
                    succeeded = (stream.eof() || stream.good()) &&
                        contents.find("### DATA ###") != std::string::npos;
                } else {
                    succeeded = false;
                }
            }

            auto* completion = new Completion{
                state,
                generation,
                succeeded,
                std::move(contents),
            };
            g_main_context_invoke(nullptr, callback, completion);
        }
    );

    if (!posted) {
        state->generation.fetch_add(1, std::memory_order_acq_rel);
        emoji_loading_ = false;
        emoji_status_message_ = "Unable to start emoji index loader";
        rebuild_results();
    }
}

void LauncherOverlay::rebuild_emoji_results() {
    if (search_mode_ != SearchMode::Emoji) return;

    if (emoji_loading_ && !emoji_database_loaded_) {
        current_results_.clear();
        emoji_status_message_ = "Loading emoji index…";
        rebuild_results();
        return;
    }

    emoji_status_message_.clear();
    if (emoji_page_growth_idle_id_ != 0) {
        g_source_remove(emoji_page_growth_idle_id_);
        emoji_page_growth_idle_id_ = 0;
    }
    emoji_all_results_ = services::launcher_emoji_results(
        emoji_database_text_,
        emoji_filter_,
        kEmojiBrowseResultCount
    );
    emoji_rendered_count_ = std::min(
        kEmojiInitialResultCount,
        emoji_all_results_.size()
    );
    current_results_.assign(
        emoji_all_results_.begin(),
        emoji_all_results_.begin() +
            static_cast<std::ptrdiff_t>(emoji_rendered_count_)
    );
    current_results_.reserve(emoji_all_results_.size());
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(results_scroller_)),
        0.0
    );
    rebuild_results();
    schedule_emoji_page_growth();
}

bool LauncherOverlay::append_next_emoji_page(
    std::size_t minimum_result_count
) {
    if (search_mode_ != SearchMode::Emoji ||
        (emoji_loading_ && !emoji_database_loaded_) ||
        emoji_rendered_count_ >= emoji_all_results_.size()) {
        return false;
    }

    const std::size_t target_count = std::min(
        emoji_all_results_.size(),
        std::max(
            emoji_rendered_count_ + kEmojiResultBatchCount,
            minimum_result_count
        )
    );
    const std::size_t old_count = emoji_rendered_count_;
    current_results_.reserve(target_count);
    for (std::size_t index = old_count; index < target_count; ++index) {
        current_results_.push_back(emoji_all_results_[index]);
        append_result_row(current_results_.back());
    }
    emoji_rendered_count_ = target_count;
    return emoji_rendered_count_ > old_count;
}

void LauncherOverlay::schedule_emoji_page_growth() {
    if (search_mode_ != SearchMode::Emoji ||
        (emoji_loading_ && !emoji_database_loaded_) ||
        emoji_rendered_count_ >= emoji_all_results_.size() ||
        emoji_page_growth_idle_id_ != 0 || results_scroller_ == nullptr) {
        return;
    }

    GtkAdjustment* adjustment = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(results_scroller_)
    );
    const double page_size = gtk_adjustment_get_page_size(adjustment);
    const double upper = gtk_adjustment_get_upper(adjustment);
    const double value = gtk_adjustment_get_value(adjustment);
    constexpr double load_ahead_distance = 180.0;
    if (page_size <= 1.0 || value + page_size + load_ahead_distance < upper) {
        return;
    }

    emoji_page_growth_idle_id_ = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        +[](gpointer data) -> gboolean {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            overlay->emoji_page_growth_idle_id_ = 0;
            if (overlay->append_next_emoji_page()) {
                overlay->schedule_emoji_page_growth();
            }
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

void LauncherOverlay::request_clipboard_history() {
    auto state = clipboard_async_state_;
    const std::uint64_t generation =
        state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    const bool had_cached_history = clipboard_history_loaded_;

    clipboard_loading_ = true;
    if (!had_cached_history) {
        clipboard_history_loaded_ = false;
        clipboard_status_message_ = "Loading clipboard history…";
        clipboard_all_results_.clear();
        clipboard_rendered_count_ = 0;
        current_results_.clear();
        rebuild_results();
    }

    struct Completion {
        std::shared_ptr<ClipboardAsyncState> state;
        std::uint64_t generation = 0;
        bool had_cached_history = false;
        realmheart::core::CommandResult result;
    };

    const auto callback = +[](gpointer data) -> gboolean {
        std::unique_ptr<Completion> completion(static_cast<Completion*>(data));
        LauncherOverlay* owner = completion->state->owner.load(
            std::memory_order_acquire
        );
        if (owner == nullptr ||
            completion->generation != completion->state->generation.load(
                std::memory_order_acquire
            ) || owner->search_mode_ != SearchMode::Clipboard) {
            return G_SOURCE_REMOVE;
        }

        owner->clipboard_loading_ = false;
        if (!completion->result.succeeded()) {
            if (!completion->had_cached_history) {
                owner->clipboard_history_output_.clear();
                owner->clipboard_history_loaded_ = false;
                owner->clipboard_status_message_ = "Unable to read clipboard history";
                owner->current_results_.clear();
                owner->rebuild_results();
            }
            return G_SOURCE_REMOVE;
        }

        const bool changed = owner->clipboard_history_output_ != completion->result.output;
        owner->clipboard_history_output_ = std::move(completion->result.output);
        owner->clipboard_history_loaded_ = true;
        owner->clipboard_status_message_.clear();
        if (changed || !completion->had_cached_history) {
            owner->rebuild_clipboard_results();
        }
        return G_SOURCE_REMOVE;
    };

    const bool posted = realmheart::core::shared_task_executor().post(
        [state, generation, had_cached_history, callback] {
            realmheart::core::CommandOptions options;
            options.deadline = std::chrono::milliseconds(1800);
            options.max_output_bytes = 256U * 1024U;
            auto* completion = new Completion{
                state,
                generation,
                had_cached_history,
                realmheart::core::run_capture({"cliphist", "list"}, options),
            };
            g_main_context_invoke(nullptr, callback, completion);
        }
    );

    if (!posted) {
        state->generation.fetch_add(1, std::memory_order_acq_rel);
        clipboard_loading_ = false;
        if (!had_cached_history) {
            clipboard_status_message_ = "Unable to start clipboard history loader";
            rebuild_results();
        }
    }
}

void LauncherOverlay::rebuild_clipboard_results() {
    if (search_mode_ != SearchMode::Clipboard) return;

    if (clipboard_loading_ && !clipboard_history_loaded_) {
        current_results_.clear();
        clipboard_status_message_ = "Loading clipboard history…";
        rebuild_results();
        return;
    }

    clipboard_status_message_.clear();
    if (clipboard_page_growth_idle_id_ != 0) {
        g_source_remove(clipboard_page_growth_idle_id_);
        clipboard_page_growth_idle_id_ = 0;
    }
    clipboard_all_results_ = services::launcher_clipboard_results(
        clipboard_history_output_,
        clipboard_filter_,
        kClipboardBrowseResultCount
    );
    clipboard_rendered_count_ = std::min(
        kClipboardInitialResultCount,
        clipboard_all_results_.size()
    );
    current_results_.assign(
        clipboard_all_results_.begin(),
        clipboard_all_results_.begin() +
            static_cast<std::ptrdiff_t>(clipboard_rendered_count_)
    );
    current_results_.reserve(clipboard_all_results_.size());
    gtk_adjustment_set_value(
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(results_scroller_)),
        0.0
    );
    rebuild_results();
    schedule_clipboard_page_growth();
}

bool LauncherOverlay::append_next_clipboard_page(
    std::size_t minimum_result_count
) {
    if (search_mode_ != SearchMode::Clipboard ||
        (clipboard_loading_ && !clipboard_history_loaded_) ||
        clipboard_rendered_count_ >= clipboard_all_results_.size()) {
        return false;
    }

    const std::size_t target_count = std::min(
        clipboard_all_results_.size(),
        std::max(
            clipboard_rendered_count_ + kClipboardResultBatchCount,
            minimum_result_count
        )
    );
    const std::size_t old_count = clipboard_rendered_count_;
    current_results_.reserve(target_count);
    for (std::size_t index = old_count; index < target_count; ++index) {
        current_results_.push_back(clipboard_all_results_[index]);
        append_result_row(current_results_.back());
    }
    clipboard_rendered_count_ = target_count;
    schedule_visible_clipboard_thumbnails();
    return clipboard_rendered_count_ > old_count;
}

void LauncherOverlay::schedule_clipboard_page_growth() {
    if (search_mode_ != SearchMode::Clipboard ||
        (clipboard_loading_ && !clipboard_history_loaded_) ||
        clipboard_rendered_count_ >= clipboard_all_results_.size() ||
        clipboard_page_growth_idle_id_ != 0 || results_scroller_ == nullptr) {
        return;
    }

    GtkAdjustment* adjustment = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(results_scroller_)
    );
    const double page_size = gtk_adjustment_get_page_size(adjustment);
    const double upper = gtk_adjustment_get_upper(adjustment);
    const double value = gtk_adjustment_get_value(adjustment);
    constexpr double load_ahead_distance = 180.0;
    if (page_size <= 1.0 || value + page_size + load_ahead_distance < upper) {
        return;
    }

    clipboard_page_growth_idle_id_ = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        +[](gpointer data) -> gboolean {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            overlay->clipboard_page_growth_idle_id_ = 0;
            if (overlay->append_next_clipboard_page()) {
                overlay->schedule_clipboard_page_growth();
            }
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

void LauncherOverlay::cache_clipboard_thumbnail(
    std::string id,
    ClipboardThumbnail thumbnail
) {
    if (thumbnail.texture == nullptr || id.empty()) return;

    if (const auto existing = clipboard_thumbnail_cache_.find(id);
        existing != clipboard_thumbnail_cache_.end()) {
        if (existing->second.texture != nullptr) g_object_unref(existing->second.texture);
        clipboard_thumbnail_cache_.erase(existing);
        std::erase(clipboard_thumbnail_lru_, id);
    }

    while (clipboard_thumbnail_cache_.size() >= kClipboardThumbnailCacheLimit &&
           !clipboard_thumbnail_lru_.empty()) {
        const std::string evicted = std::move(clipboard_thumbnail_lru_.front());
        clipboard_thumbnail_lru_.erase(clipboard_thumbnail_lru_.begin());
        const auto found = clipboard_thumbnail_cache_.find(evicted);
        if (found == clipboard_thumbnail_cache_.end()) continue;
        if (found->second.texture != nullptr) g_object_unref(found->second.texture);
        clipboard_thumbnail_cache_.erase(found);
    }

    clipboard_thumbnail_lru_.push_back(id);
    clipboard_thumbnail_cache_.emplace(std::move(id), std::move(thumbnail));
}

void LauncherOverlay::apply_clipboard_thumbnail(std::string_view id) {
    const auto row = clipboard_rows_.find(std::string(id));
    const auto cached = clipboard_thumbnail_cache_.find(std::string(id));
    if (row == clipboard_rows_.end() || cached == clipboard_thumbnail_cache_.end() ||
        row->second.view_generation != clipboard_view_generation_ ||
        row->second.icon_slot == nullptr || cached->second.texture == nullptr) {
        return;
    }

    gtk_overlay_set_child(GTK_OVERLAY(row->second.icon_slot), nullptr);

    GtkWidget* picture = gtk_picture_new_for_paintable(
        GDK_PAINTABLE(cached->second.texture)
    );
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_widget_set_size_request(
        picture,
        kClipboardThumbnailWidth,
        kClipboardThumbnailHeight
    );
    gtk_widget_set_hexpand(picture, TRUE);
    gtk_widget_set_vexpand(picture, TRUE);
    gtk_widget_set_halign(picture, GTK_ALIGN_FILL);
    gtk_widget_set_valign(picture, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(picture, FALSE);
    gtk_widget_add_css_class(picture, "realmheart-launcher-clipboard-thumbnail-image");
    gtk_overlay_set_child(GTK_OVERLAY(row->second.icon_slot), picture);
    row->second.thumbnail_visible = true;

    if (row->second.subtitle != nullptr) {
        std::string subtitle = "Image · " +
            std::to_string(cached->second.source_width) + "×" +
            std::to_string(cached->second.source_height);
        if (!cached->second.format.empty()) subtitle += " · " + cached->second.format;
        gtk_label_set_text(GTK_LABEL(row->second.subtitle), subtitle.c_str());
    }

    if (selected_result_row_ != nullptr) {
        retarget_result_selection(selected_result_row_);
    }
}

void LauncherOverlay::schedule_visible_clipboard_thumbnails() {
    if (search_mode_ != SearchMode::Clipboard ||
        clipboard_thumbnail_visibility_idle_id_ != 0) {
        return;
    }

    clipboard_thumbnail_visibility_idle_id_ = g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        +[](gpointer data) -> gboolean {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            overlay->clipboard_thumbnail_visibility_idle_id_ = 0;
            overlay->request_visible_clipboard_thumbnails();
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

void LauncherOverlay::request_visible_clipboard_thumbnails() {
    if (search_mode_ != SearchMode::Clipboard || results_scroller_ == nullptr ||
        results_list_ == nullptr) {
        return;
    }

    GtkAdjustment* adjustment = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(results_scroller_)
    );
    const double viewport_top = gtk_adjustment_get_value(adjustment);
    const double page_size = std::max(1.0, gtk_adjustment_get_page_size(adjustment));
    constexpr double prefetch_margin = 140.0;
    const double visible_top = std::max(0.0, viewport_top - prefetch_margin);
    const double visible_bottom = viewport_top + page_size + prefetch_margin;

    // First release pictures that have moved well outside the viewport. The
    // small texture cache keeps recent previews reusable, but offscreen rows do
    // not retain their own paintable references indefinitely.
    for (auto& [id, widgets] : clipboard_rows_) {
        static_cast<void>(id);
        if (widgets.view_generation != clipboard_view_generation_ ||
            widgets.row == nullptr) {
            continue;
        }

        graphene_rect_t bounds{};
        if (!gtk_widget_compute_bounds(widgets.row, results_list_, &bounds)) continue;
        const double row_top = bounds.origin.y;
        const double row_bottom = row_top + bounds.size.height;
        const bool visible = row_bottom >= visible_top && row_top <= visible_bottom;
        if (visible || !widgets.thumbnail_visible || widgets.icon_slot == nullptr) {
            continue;
        }

        gtk_overlay_set_child(GTK_OVERLAY(widgets.icon_slot), nullptr);
        GtkWidget* placeholder = make_launcher_icon(
            "Realmheart-Icons/clip-history.svg",
            30
        );
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        gtk_overlay_set_child(GTK_OVERLAY(widgets.icon_slot), placeholder);
        widgets.thumbnail_visible = false;
    }

    // Never let frantic scrolling enqueue an unbounded tail of cliphist
    // subprocesses. Two jobs match the shared executor's worker count; each
    // completion schedules the next visible row.
    if (clipboard_thumbnail_active_jobs_ >= kClipboardThumbnailMaximumJobs) return;

    for (const auto& result : current_results_) {
        if (!result.clipboard_image) continue;
        const auto found = clipboard_rows_.find(result.id);
        if (found == clipboard_rows_.end() ||
            found->second.view_generation != clipboard_view_generation_ ||
            found->second.row == nullptr) {
            continue;
        }

        graphene_rect_t bounds{};
        if (!gtk_widget_compute_bounds(found->second.row, results_list_, &bounds)) {
            continue;
        }
        const double row_top = bounds.origin.y;
        const double row_bottom = row_top + bounds.size.height;
        if (row_bottom < visible_top || row_top > visible_bottom) continue;

        request_clipboard_thumbnail(result);
        if (clipboard_thumbnail_active_jobs_ >= kClipboardThumbnailMaximumJobs) break;
    }
}

void LauncherOverlay::ensure_result_row_visible(GtkListBoxRow* row) {
    if (row == nullptr || results_scroller_ == nullptr || results_list_ == nullptr) {
        return;
    }

    graphene_rect_t bounds{};
    if (!gtk_widget_compute_bounds(GTK_WIDGET(row), results_list_, &bounds)) return;

    GtkAdjustment* adjustment = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(results_scroller_)
    );
    const double page_size = gtk_adjustment_get_page_size(adjustment);
    if (page_size <= 1.0) return;

    const double current = gtk_adjustment_get_value(adjustment);
    const double row_top = bounds.origin.y;
    const double row_bottom = row_top + bounds.size.height;
    double target = current;
    if (row_top < current) {
        target = row_top;
    } else if (row_bottom > current + page_size) {
        target = row_bottom - page_size;
    }

    const double lower = gtk_adjustment_get_lower(adjustment);
    const double upper = std::max(
        lower,
        gtk_adjustment_get_upper(adjustment) - page_size
    );
    target = std::clamp(target, lower, upper);
    if (std::abs(target - current) > 0.5) gtk_adjustment_set_value(adjustment, target);

    retarget_result_selection(row);
    schedule_visible_clipboard_thumbnails();
}

void LauncherOverlay::request_clipboard_thumbnail(
    const services::LauncherResult& result
) {
    if (!result.clipboard_image || result.id.empty() ||
        search_mode_ != SearchMode::Clipboard) {
        return;
    }
    if (clipboard_thumbnail_cache_.contains(result.id)) {
        apply_clipboard_thumbnail(result.id);
        return;
    }

    auto state = clipboard_async_state_;
    const std::uint64_t generation = state->generation.load(std::memory_order_acquire);
    if (const auto existing = clipboard_thumbnail_inflight_.find(result.id);
        existing != clipboard_thumbnail_inflight_.end() &&
        existing->second == generation) {
        return;
    }
    if (clipboard_thumbnail_active_jobs_ >= kClipboardThumbnailMaximumJobs) return;

    const std::string id = result.id;
    const std::string fallback_format = result.clipboard_mime;
    clipboard_thumbnail_inflight_[id] = generation;
    ++clipboard_thumbnail_active_jobs_;

    struct Completion {
        std::shared_ptr<ClipboardAsyncState> state;
        std::uint64_t generation = 0;
        std::string id;
        std::string fallback_format;
        std::optional<DecodedClipboardThumbnail> decoded;
    };

    const auto callback = +[](gpointer data) -> gboolean {
        std::unique_ptr<Completion> completion(static_cast<Completion*>(data));
        LauncherOverlay* owner = completion->state->owner.load(
            std::memory_order_acquire
        );
        if (owner == nullptr) return G_SOURCE_REMOVE;

        if (owner->clipboard_thumbnail_active_jobs_ > 0) {
            --owner->clipboard_thumbnail_active_jobs_;
        }
        if (const auto inflight = owner->clipboard_thumbnail_inflight_.find(
                completion->id
            ); inflight != owner->clipboard_thumbnail_inflight_.end() &&
            inflight->second == completion->generation) {
            owner->clipboard_thumbnail_inflight_.erase(inflight);
        }

        const bool request_is_current =
            completion->generation == completion->state->generation.load(
                std::memory_order_acquire
            ) && owner->search_mode_ == SearchMode::Clipboard;
        if (request_is_current && completion->decoded.has_value()) {
            auto& decoded = *completion->decoded;
            const gsize byte_count = static_cast<gsize>(decoded.rowstride) *
                static_cast<gsize>(decoded.height);
            GBytes* bytes = g_bytes_new(decoded.pixels.data(), byte_count);
            GdkTexture* texture = gdk_memory_texture_new(
                decoded.width,
                decoded.height,
                decoded.has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                bytes,
                static_cast<gsize>(decoded.rowstride)
            );
            g_bytes_unref(bytes);

            if (texture != nullptr) {
                std::string format = uppercase_ascii(decoded.format);
                if (format.empty()) {
                    const std::size_t slash = completion->fallback_format.find('/');
                    format = uppercase_ascii(
                        slash == std::string::npos
                            ? completion->fallback_format
                            : completion->fallback_format.substr(slash + 1)
                    );
                }

                owner->cache_clipboard_thumbnail(
                    completion->id,
                    ClipboardThumbnail{
                        texture,
                        decoded.source_width,
                        decoded.source_height,
                        std::move(format),
                    }
                );
                owner->apply_clipboard_thumbnail(completion->id);
            }
        }

        // Whether the completed request was current or stale, a worker slot is
        // free now. Continue filling only the rows visible in the current view.
        owner->schedule_visible_clipboard_thumbnails();
        return G_SOURCE_REMOVE;
    };

    const bool posted = realmheart::core::shared_task_executor().post(
        [state, generation, id, fallback_format, callback] {
            realmheart::core::CommandOptions options;
            options.deadline = std::chrono::milliseconds(2500);
            options.max_output_bytes = kClipboardMaximumDecodedBytes;
            const auto decoded_bytes = realmheart::core::run_capture(
                {"cliphist", "decode", id},
                options
            );

            std::optional<DecodedClipboardThumbnail> decoded;
            if (decoded_bytes.succeeded() && !decoded_bytes.truncated) {
                decoded = decode_clipboard_thumbnail(decoded_bytes.output);
            }
            auto* completion = new Completion{
                state,
                generation,
                id,
                fallback_format,
                std::move(decoded),
            };
            g_main_context_invoke(nullptr, callback, completion);
        }
    );

    if (!posted) {
        if (clipboard_thumbnail_active_jobs_ > 0) --clipboard_thumbnail_active_jobs_;
        if (const auto inflight = clipboard_thumbnail_inflight_.find(id);
            inflight != clipboard_thumbnail_inflight_.end() &&
            inflight->second == generation) {
            clipboard_thumbnail_inflight_.erase(inflight);
        }
    }
}

void LauncherOverlay::request_clipboard_wipe() {
    auto state = clipboard_async_state_;
    const std::uint64_t generation =
        state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;

    clipboard_loading_ = true;
    clipboard_clear_armed_ = false;
    clipboard_status_message_ = "Clearing clipboard history…";
    clipboard_all_results_.clear();
    clipboard_rendered_count_ = 0;
    current_results_.clear();
    clear_clipboard_thumbnail_cache();
    rebuild_results();

    struct Completion {
        std::shared_ptr<ClipboardAsyncState> state;
        std::uint64_t generation = 0;
        realmheart::core::CommandResult result;
    };

    const auto callback = +[](gpointer data) -> gboolean {
        std::unique_ptr<Completion> completion(static_cast<Completion*>(data));
        LauncherOverlay* owner = completion->state->owner.load(
            std::memory_order_acquire
        );
        if (owner == nullptr ||
            completion->generation != completion->state->generation.load(
                std::memory_order_acquire
            ) || owner->search_mode_ != SearchMode::ClipboardClear) {
            return G_SOURCE_REMOVE;
        }

        owner->clipboard_loading_ = false;
        owner->clipboard_history_output_.clear();
        owner->clipboard_history_loaded_ = false;
        owner->clipboard_all_results_.clear();
        owner->clipboard_rendered_count_ = 0;
        if (!completion->result.succeeded()) {
            owner->clipboard_status_message_ = "Unable to clear clipboard history";
            owner->clipboard_clear_armed_ = false;
            auto retry = services::launcher_clipboard_clear_result(false);
            retry.subtitle = "Unable to clear history · Press Enter to retry";
            owner->current_results_ = {std::move(retry)};
            owner->rebuild_results();
            return G_SOURCE_REMOVE;
        }

        gtk_editable_set_text(GTK_EDITABLE(owner->search_entry_), ">clip ");
        gtk_editable_set_position(GTK_EDITABLE(owner->search_entry_), -1);
        return G_SOURCE_REMOVE;
    };

    const bool posted = realmheart::core::shared_task_executor().post(
        [state, generation, callback] {
            realmheart::core::CommandOptions options;
            options.deadline = std::chrono::milliseconds(1800);
            options.max_output_bytes = 32U * 1024U;
            auto* completion = new Completion{
                state,
                generation,
                realmheart::core::run_capture({"cliphist", "wipe"}, options),
            };
            g_main_context_invoke(nullptr, callback, completion);
        }
    );

    if (!posted) {
        state->generation.fetch_add(1, std::memory_order_acq_rel);
        clipboard_loading_ = false;
        clipboard_status_message_ = "Unable to start clipboard history clear";
        auto retry = services::launcher_clipboard_clear_result(false);
        retry.subtitle = "Unable to start clear operation · Press Enter to retry";
        current_results_ = {std::move(retry)};
        rebuild_results();
    }
}

void LauncherOverlay::request_clipboard_delete(std::string id) {
    if (search_mode_ != SearchMode::Clipboard || id.empty()) return;

    const auto found = std::ranges::find_if(
        clipboard_all_results_,
        [&id](const services::LauncherResult& result) {
            return result.kind == services::LauncherResultKind::Clipboard &&
                result.id == id;
        }
    );
    if (found == clipboard_all_results_.end()) return;

    const services::LauncherResult deleted = *found;
    const auto argv = services::launcher_clipboard_delete_argv(
        deleted.id,
        deleted.description
    );
    if (argv.empty()) return;

    auto state = clipboard_async_state_;
    const std::uint64_t generation =
        state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;

    std::size_t deleted_index = 0;
    if (const auto visible = std::ranges::find_if(
            current_results_,
            [&id](const services::LauncherResult& result) {
                return result.kind == services::LauncherResultKind::Clipboard &&
                    result.id == id;
            }
        ); visible != current_results_.end()) {
        deleted_index = static_cast<std::size_t>(std::distance(
            current_results_.begin(),
            visible
        ));
    }

    static_cast<void>(erase_cliphist_entry_line(clipboard_history_output_, id));
    std::erase_if(
        clipboard_all_results_,
        [&id](const services::LauncherResult& result) { return result.id == id; }
    );

    if (const auto cached = clipboard_thumbnail_cache_.find(id);
        cached != clipboard_thumbnail_cache_.end()) {
        if (cached->second.texture != nullptr) g_object_unref(cached->second.texture);
        clipboard_thumbnail_cache_.erase(cached);
    }
    std::erase(clipboard_thumbnail_lru_, id);

    clipboard_rendered_count_ = std::min(
        clipboard_rendered_count_,
        clipboard_all_results_.size()
    );
    current_results_.assign(
        clipboard_all_results_.begin(),
        clipboard_all_results_.begin() +
            static_cast<std::ptrdiff_t>(clipboard_rendered_count_)
    );
    current_results_.reserve(clipboard_all_results_.size());
    rebuild_results();

    if (!current_results_.empty()) {
        const int next_index = static_cast<int>(std::min(
            deleted_index,
            current_results_.size() - 1
        ));
        if (GtkListBoxRow* next = gtk_list_box_get_row_at_index(
                GTK_LIST_BOX(results_list_),
                next_index
            ); next != nullptr) {
            gtk_list_box_select_row(GTK_LIST_BOX(results_list_), next);
            ensure_result_row_visible(next);
        }
    }
    schedule_clipboard_page_growth();

    struct Completion {
        std::shared_ptr<ClipboardAsyncState> state;
        std::uint64_t generation = 0;
        realmheart::core::CommandResult result;
    };

    const auto callback = +[](gpointer data) -> gboolean {
        std::unique_ptr<Completion> completion(static_cast<Completion*>(data));
        LauncherOverlay* owner = completion->state->owner.load(
            std::memory_order_acquire
        );
        if (owner == nullptr ||
            completion->generation != completion->state->generation.load(
                std::memory_order_acquire
            ) || owner->search_mode_ != SearchMode::Clipboard) {
            return G_SOURCE_REMOVE;
        }

        if (!completion->result.succeeded()) {
            // The row disappeared optimistically. Reload the authoritative
            // database so a failed deletion restores it instead of lying.
            owner->request_clipboard_history();
        }
        return G_SOURCE_REMOVE;
    };

    const bool posted = realmheart::core::shared_task_executor().post(
        [state, generation, argv, callback] {
            realmheart::core::CommandOptions options;
            options.deadline = std::chrono::milliseconds(1800);
            options.max_output_bytes = 32U * 1024U;
            auto* completion = new Completion{
                state,
                generation,
                realmheart::core::run_capture(argv, options),
            };
            g_main_context_invoke(nullptr, callback, completion);
        }
    );

    if (!posted) request_clipboard_history();
}

void LauncherOverlay::activate_clipboard_action(
    const services::LauncherResult& result
) {
    if (result.id != "clear-history") return;
    if (!clipboard_clear_armed_) {
        clipboard_clear_armed_ = true;
        current_results_ = {services::launcher_clipboard_clear_result(true)};
        rebuild_results();
        return;
    }
    request_clipboard_wipe();
}


GtkListBoxRow* LauncherOverlay::append_result_row(
    const services::LauncherResult& result
) {
    GtkWidget* row = gtk_list_box_row_new();
    gtk_widget_add_css_class(row, "realmheart-launcher-result-row");

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);

    GtkWidget* icon = nullptr;
    GtkWidget* clipboard_icon_slot = nullptr;
    if (result.kind == services::LauncherResultKind::Emoji) {
        icon = gtk_label_new(result.id.c_str());
        gtk_widget_set_size_request(icon, 42, 42);
        gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(icon, "realmheart-launcher-emoji-glyph");
    } else if (result.kind == services::LauncherResultKind::Clipboard &&
        result.clipboard_image) {
        clipboard_icon_slot = gtk_overlay_new();
        gtk_widget_set_size_request(
            clipboard_icon_slot,
            kClipboardThumbnailWidth,
            kClipboardThumbnailHeight
        );
        gtk_widget_set_halign(clipboard_icon_slot, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(clipboard_icon_slot, GTK_ALIGN_CENTER);
        gtk_widget_set_overflow(clipboard_icon_slot, GTK_OVERFLOW_HIDDEN);
        gtk_widget_add_css_class(
            clipboard_icon_slot,
            "realmheart-launcher-clipboard-thumbnail"
        );
        GtkWidget* placeholder = make_launcher_icon(result.icon_name, 30);
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        gtk_overlay_set_child(GTK_OVERLAY(clipboard_icon_slot), placeholder);
        icon = clipboard_icon_slot;
    } else {
        icon = make_launcher_icon(result.icon_name, 36);
    }

    GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(labels, TRUE);

    GtkWidget* title = gtk_label_new(result.title.c_str());
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title, "realmheart-launcher-row-title");

    GtkWidget* subtitle = gtk_label_new(result.subtitle.c_str());
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(subtitle, "realmheart-launcher-row-subtitle");
    gtk_widget_set_visible(subtitle, !result.subtitle.empty());

    GtkWidget* trailing = nullptr;
    if (result.kind == services::LauncherResultKind::Clipboard) {
        GtkWidget* remove = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(remove), FALSE);
        gtk_widget_set_focusable(remove, FALSE);
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(remove, "realmheart-launcher-clipboard-delete");
        gtk_widget_set_tooltip_text(remove, "Delete this clipboard entry");

        auto* remove_icon = new bar::widgets::ThemedSvgIcon(
            "Realmheart-Icons/trash.svg",
            18
        );
        gtk_button_set_child(GTK_BUTTON(remove), remove_icon->widget());
        g_object_set_data_full(
            G_OBJECT(remove),
            "realmheart-delete-icon",
            remove_icon,
            +[](gpointer data) {
                delete static_cast<bar::widgets::ThemedSvgIcon*>(data);
            }
        );
        g_object_set_data_full(
            G_OBJECT(remove),
            "realmheart-clipboard-id",
            g_strdup(result.id.c_str()),
            g_free
        );
        g_signal_connect(remove, "clicked", G_CALLBACK(+[](
            GtkButton* source,
            gpointer data
        ) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            const char* id = static_cast<const char*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-clipboard-id"
            ));
            if (id != nullptr) overlay->request_clipboard_delete(id);
        }), this);
        trailing = remove;
    } else {
        GtkWidget* kind = gtk_label_new(result_kind_label(result.kind));
        gtk_widget_set_valign(kind, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(kind, "realmheart-launcher-result-kind");
        if (result.kind == services::LauncherResultKind::ClipboardAction) {
            gtk_widget_add_css_class(row, "destructive");
            gtk_widget_add_css_class(kind, "destructive");
        }
        trailing = kind;
    }

    gtk_box_append(GTK_BOX(labels), title);
    gtk_box_append(GTK_BOX(labels), subtitle);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), labels);
    gtk_box_append(GTK_BOX(box), trailing);

    if (result.kind == services::LauncherResultKind::Application) {
        const bool pinned = constellation_contains(result.id);
        GtkWidget* pin = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(pin), FALSE);
        auto* pin_icon = new bar::widgets::ThemedSvgIcon(
            pinned
                ? "Realmheart-Icons/subtract.svg"
                : "Realmheart-Icons/add.svg",
            18
        );
        gtk_button_set_child(GTK_BUTTON(pin), pin_icon->widget());
        g_object_set_data_full(
            G_OBJECT(pin),
            "realmheart-pin-icon",
            pin_icon,
            +[](gpointer data) {
                delete static_cast<bar::widgets::ThemedSvgIcon*>(data);
            }
        );
        gtk_widget_set_valign(pin, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(pin, "realmheart-launcher-result-pin");
        gtk_widget_set_tooltip_text(
            pin,
            pinned ? "Unpin from launcher" : "Pin to launcher"
        );
        g_object_set_data_full(
            G_OBJECT(pin),
            "realmheart-application-id",
            g_strdup(result.id.c_str()),
            g_free
        );
        g_signal_connect(pin, "clicked", G_CALLBACK(+[](GtkButton* source, gpointer data) {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            const char* application_id = static_cast<const char*>(g_object_get_data(
                G_OBJECT(source),
                "realmheart-application-id"
            ));
            if (application_id == nullptr) return;

            overlay->toggle_constellation_application(application_id);
            const bool now_pinned = overlay->constellation_contains(application_id);
            auto* themed_icon = static_cast<bar::widgets::ThemedSvgIcon*>(
                g_object_get_data(G_OBJECT(source), "realmheart-pin-icon")
            );
            if (themed_icon != nullptr) {
                static_cast<void>(themed_icon->set_icon(
                    now_pinned
                        ? "Realmheart-Icons/subtract.svg"
                        : "Realmheart-Icons/add.svg"
                ));
            }
            gtk_widget_set_tooltip_text(
                GTK_WIDGET(source),
                now_pinned ? "Unpin from launcher" : "Pin to launcher"
            );
        }), this);
        gtk_box_append(GTK_BOX(box), pin);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    auto motion = std::make_unique<ResultRowMotion>();
    motion->bind(GTK_LIST_BOX_ROW(row), box);
    result_row_motions_.push_back(std::move(motion));

    GtkEventController* hover = gtk_event_controller_motion_new();
    g_signal_connect(hover, "motion", G_CALLBACK(+[](
        GtkEventController* controller, double, double, gpointer data
    ) {
        GtkWidget* widget = gtk_event_controller_get_widget(controller);
        auto* hovered_row = GTK_LIST_BOX_ROW(widget);
        auto* overlay = static_cast<LauncherOverlay*>(data);
        gtk_list_box_select_row(GTK_LIST_BOX(overlay->results_list_), hovered_row);
    }), this);
    gtk_widget_add_controller(row, hover);

    gtk_list_box_append(GTK_LIST_BOX(results_list_), row);
    if (result.kind == services::LauncherResultKind::Clipboard &&
        result.clipboard_image && clipboard_icon_slot != nullptr) {
        clipboard_rows_[result.id] = ClipboardRowWidgets{
            row,
            clipboard_icon_slot,
            subtitle,
            clipboard_view_generation_,
        };
    }
    return GTK_LIST_BOX_ROW(row);
}

void LauncherOverlay::rebuild_results() {
    selected_result_row_ = nullptr;
    result_selection_target_visible_ = false;
    result_row_motions_.clear();
    clipboard_rows_.clear();
    ++clipboard_view_generation_;
    clear_list(results_list_);

    for (const auto& result : current_results_) append_result_row(result);

    if (current_results_.empty()) {
        const bool loading_clipboard = search_mode_ == SearchMode::Clipboard &&
            clipboard_loading_ && !clipboard_history_loaded_;
        const bool loading_emoji = search_mode_ == SearchMode::Emoji &&
            emoji_loading_ && !emoji_database_loaded_;
        if (loading_clipboard || loading_emoji) {
            GtkWidget* row = gtk_list_box_row_new();
            gtk_widget_set_sensitive(row, FALSE);
            gtk_widget_add_css_class(
                row,
                loading_emoji
                    ? "realmheart-launcher-emoji-loading-row"
                    : "realmheart-launcher-clipboard-loading-row"
            );

            GtkWidget* loading_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
            gtk_widget_set_margin_start(loading_box, 18);
            gtk_widget_set_margin_end(loading_box, 18);
            gtk_widget_set_margin_top(loading_box, 18);
            gtk_widget_set_margin_bottom(loading_box, 18);

            GtkWidget* spinner = gtk_spinner_new();
            gtk_spinner_set_spinning(GTK_SPINNER(spinner), TRUE);
            gtk_widget_set_size_request(spinner, 28, 28);
            gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
            gtk_widget_add_css_class(
                spinner,
                "realmheart-launcher-clipboard-loading-spinner"
            );

            GtkWidget* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
            gtk_widget_set_hexpand(labels, TRUE);
            GtkWidget* title = gtk_label_new(
                loading_emoji
                    ? "Preparing emoji index…"
                    : "Preparing clipboard history…"
            );
            gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
            gtk_widget_add_css_class(title, "realmheart-launcher-row-title");
            GtkWidget* subtitle = gtk_label_new(
                loading_emoji
                    ? "Emoji will flow in as soon as the local index is ready"
                    : "Recent entries will flow in as soon as cliphist responds"
            );
            gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0F);
            gtk_widget_add_css_class(
                subtitle,
                "realmheart-launcher-row-subtitle"
            );
            gtk_box_append(GTK_BOX(labels), title);
            gtk_box_append(GTK_BOX(labels), subtitle);
            gtk_box_append(GTK_BOX(loading_box), spinner);
            gtk_box_append(GTK_BOX(loading_box), labels);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), loading_box);
            gtk_list_box_append(GTK_LIST_BOX(results_list_), row);

            for (int index = 0; index < 3; ++index) {
                GtkWidget* skeleton_row = gtk_list_box_row_new();
                gtk_widget_set_sensitive(skeleton_row, FALSE);
                gtk_widget_add_css_class(
                    skeleton_row,
                    loading_emoji
                        ? "realmheart-launcher-emoji-skeleton-row"
                        : "realmheart-launcher-clipboard-skeleton-row"
                );
                GtkWidget* skeleton = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
                gtk_widget_set_margin_start(skeleton, 18);
                gtk_widget_set_margin_end(skeleton, 18);
                gtk_widget_set_margin_top(skeleton, 10);
                gtk_widget_set_margin_bottom(skeleton, 10);
                GtkWidget* icon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_widget_set_size_request(icon, 36, 36);
                gtk_widget_add_css_class(
                    icon,
                    "realmheart-launcher-clipboard-skeleton-icon"
                );
                GtkWidget* bars = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
                gtk_widget_set_hexpand(bars, TRUE);
                GtkWidget* primary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_widget_set_size_request(primary, 230 - index * 24, 9);
                gtk_widget_set_halign(primary, GTK_ALIGN_START);
                gtk_widget_add_css_class(
                    primary,
                    "realmheart-launcher-clipboard-skeleton-bar"
                );
                GtkWidget* secondary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_widget_set_size_request(secondary, 150 + index * 18, 7);
                gtk_widget_set_halign(secondary, GTK_ALIGN_START);
                gtk_widget_add_css_class(
                    secondary,
                    "realmheart-launcher-clipboard-skeleton-bar"
                );
                gtk_widget_add_css_class(
                    secondary,
                    "secondary"
                );
                gtk_box_append(GTK_BOX(bars), primary);
                gtk_box_append(GTK_BOX(bars), secondary);
                gtk_box_append(GTK_BOX(skeleton), icon);
                gtk_box_append(GTK_BOX(skeleton), bars);
                gtk_list_box_row_set_child(
                    GTK_LIST_BOX_ROW(skeleton_row),
                    skeleton
                );
                gtk_list_box_append(GTK_LIST_BOX(results_list_), skeleton_row);
            }
        } else {
            GtkWidget* row = gtk_list_box_row_new();
            gtk_widget_set_sensitive(row, FALSE);
            const std::string message = empty_results_message();
            GtkWidget* empty = gtk_label_new(message.c_str());
            gtk_widget_set_margin_top(empty, 28);
            gtk_widget_set_margin_bottom(empty, 28);
            gtk_widget_add_css_class(empty, "realmheart-launcher-empty");
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), empty);
            gtk_list_box_append(GTK_LIST_BOX(results_list_), row);
        }
        set_selected_result(nullptr);
        retarget_result_selection(nullptr);
        return;
    }

    GtkListBoxRow* first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(results_list_), 0);
    gtk_list_box_select_row(GTK_LIST_BOX(results_list_), first);
    schedule_visible_clipboard_thumbnails();
}

void LauncherOverlay::on_search_changed() {
    const char* raw = gtk_editable_get_text(GTK_EDITABLE(search_entry_));
    const std::string query = raw != nullptr ? raw : "";
    const bool searching = query.find_first_not_of(" \t\n\r") != std::string::npos;

    const bool show_constellation = !searching && central_target_visible_ &&
        central_progress_ >= kConstellationRevealThreshold;
    set_constellation_visible(show_constellation);

    // The idle centre owns a strong lower shadow, but keeping that shadow while
    // the result sheet unfolds makes both surfaces merge into a muddy halo.
    // Search mode deliberately hands depth to the lower sheet instead.
    if (searching) {
        gtk_widget_add_css_class(centre_shell_, "searching");
    } else {
        gtk_widget_remove_css_class(centre_shell_, "searching");
    }
    gtk_revealer_set_reveal_child(GTK_REVEALER(results_revealer_), searching);

    if (!searching) {
        leave_clipboard_mode();
        leave_emoji_mode();
        current_results_.clear();
        selected_result_row_ = nullptr;
        result_selection_target_visible_ = false;
        result_row_motions_.clear();
        clipboard_rows_.clear();
        ++clipboard_view_generation_;
        clear_list(results_list_);
        set_selected_result(nullptr);
        retarget_result_selection(nullptr);
        return;
    }

    std::string clipboard_filter;
    if (parse_clipboard_query(query, clipboard_filter)) {
        enter_clipboard_mode(std::move(clipboard_filter));
        return;
    }
    if (parse_clipboard_clear_query(query)) {
        enter_clipboard_clear_mode();
        return;
    }
    std::string emoji_filter;
    if (parse_emoji_query(query, emoji_filter)) {
        enter_emoji_mode(std::move(emoji_filter));
        return;
    }

    leave_clipboard_mode();
    leave_emoji_mode();
    current_results_ = services::launcher_command_suggestions(query);
    if (current_results_.empty()) {
        current_results_ = service_.search(query, kResultCount);
    }
    rebuild_results();
}

void LauncherOverlay::on_result_selected(GtkListBoxRow* row) {
    if (row == nullptr) {
        set_selected_result(nullptr);
        retarget_result_selection(nullptr);
        return;
    }
    const int index = gtk_list_box_row_get_index(row);
    if (index < 0 || static_cast<std::size_t>(index) >= current_results_.size()) {
        set_selected_result(nullptr);
        retarget_result_selection(nullptr);
        return;
    }
    set_selected_result(&current_results_[static_cast<std::size_t>(index)]);
    retarget_result_selection(row);
}

void LauncherOverlay::set_selected_result(const services::LauncherResult* result) {
    if (result == nullptr) {
        selected_result_.reset();
        return;
    }
    selected_result_ = *result;
}

LauncherOverlay::ResultRowMotion* LauncherOverlay::result_row_motion(
    GtkListBoxRow* row
) {
    if (row == nullptr) return nullptr;
    const auto found = std::find_if(
        result_row_motions_.begin(),
        result_row_motions_.end(),
        [row](const auto& motion) {
            return motion->row != nullptr && motion->row == row;
        }
    );
    return found != result_row_motions_.end() ? found->get() : nullptr;
}

bool LauncherOverlay::update_result_selection_target() {
    if (selected_result_row_ == nullptr || results_overlay_ == nullptr) {
        return false;
    }

    graphene_rect_t bounds{};
    if (!gtk_widget_compute_bounds(
            GTK_WIDGET(selected_result_row_),
            results_overlay_,
            &bounds
        ) || bounds.size.width <= 1.0F || bounds.size.height <= 1.0F) {
        return false;
    }

    result_selection_target_x_ = bounds.origin.x;
    result_selection_target_y_ = bounds.origin.y;
    result_selection_target_width_ = bounds.size.width;
    result_selection_target_height_ = bounds.size.height;
    return true;
}

void LauncherOverlay::retarget_result_selection(GtkListBoxRow* row) {
    const bool was_visible = result_selection_target_visible_;
    const bool changed = selected_result_row_ != row;
    selected_result_row_ = row;
    result_selection_target_visible_ = row != nullptr;

    if (row != nullptr && update_result_selection_target()) {
        if (!result_selection_initialized_) {
            result_selection_y_ = result_selection_target_y_;
            result_selection_height_ = result_selection_target_height_;
            result_selection_velocity_y_ = 0.0;
            result_selection_velocity_height_ = 0.0;
            result_selection_opacity_ = 0.0;
            result_selection_initialized_ = true;
            gtk_widget_set_visible(result_selection_indicator_, TRUE);
        } else if (was_visible && changed) {
            // Preserve existing momentum for rapid key repeats, but add a small
            // directional impulse so long jumps remain responsive rather than
            // feeling like the highlight is dragging through syrup.
            result_selection_velocity_y_ += std::clamp(
                (result_selection_target_y_ - result_selection_y_) *
                    kResultSelectionTravelImpulse,
                -980.0,
                980.0
            );

            // The arriving row performs a restrained upward hop. This mirrors
            // the detached constellation's lift without making a dense result
            // list bounce around or change its layout height.
            if (ResultRowMotion* motion = result_row_motion(row); motion != nullptr) {
                motion->velocity = std::max(
                    motion->velocity,
                    kResultRowLiftImpulse
                );
            }
        }
    }

    schedule_result_selection_frame();
}

void LauncherOverlay::schedule_result_selection_frame() {
    if (result_selection_tick_id_ != 0 || root_ == nullptr) return;
    result_selection_last_frame_time_ = 0;
    result_selection_tick_id_ = gtk_widget_add_tick_callback(
        root_,
        +[](GtkWidget*, GdkFrameClock* frame_clock, gpointer data) -> gboolean {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            if (overlay->advance_result_selection_frame(frame_clock)) {
                return G_SOURCE_CONTINUE;
            }
            overlay->result_selection_tick_id_ = 0;
            overlay->result_selection_last_frame_time_ = 0;
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

bool LauncherOverlay::advance_result_selection_frame(GdkFrameClock* frame_clock) {
    if (result_selection_indicator_ == nullptr || results_overlay_ == nullptr) {
        return false;
    }

    const gint64 frame_time = gdk_frame_clock_get_frame_time(frame_clock);
    double elapsed = 1.0 / 60.0;
    if (result_selection_last_frame_time_ != 0) {
        elapsed = static_cast<double>(
            frame_time - result_selection_last_frame_time_
        ) / 1'000'000.0;
    }
    result_selection_last_frame_time_ = frame_time;
    elapsed = std::clamp(elapsed, 1.0 / 240.0, 0.05);

    const bool target_ready = result_selection_target_visible_ &&
        update_result_selection_target();
    if (target_ready && !result_selection_initialized_) {
        result_selection_y_ = result_selection_target_y_;
        result_selection_height_ = result_selection_target_height_;
        result_selection_velocity_y_ = 0.0;
        result_selection_velocity_height_ = 0.0;
        result_selection_opacity_ = 0.0;
        result_selection_initialized_ = true;
        gtk_widget_set_visible(result_selection_indicator_, TRUE);
    }

    bool animation_active = result_selection_target_visible_ && !target_ready;
    if (result_selection_initialized_ && target_ready) {
        const int step_count = std::max(
            1,
            static_cast<int>(std::ceil(elapsed / (1.0 / 120.0)))
        );
        const double step = elapsed / static_cast<double>(step_count);
        for (int index = 0; index < step_count; ++index) {
            const double y_acceleration =
                (result_selection_target_y_ - result_selection_y_) *
                    kResultSelectionSpring -
                result_selection_velocity_y_ * kResultSelectionDamping;
            const double height_acceleration =
                (result_selection_target_height_ - result_selection_height_) *
                    kResultSelectionHeightSpring -
                result_selection_velocity_height_ *
                    kResultSelectionHeightDamping;

            result_selection_velocity_y_ = std::clamp(
                result_selection_velocity_y_ + y_acceleration * step,
                -3200.0,
                3200.0
            );
            result_selection_velocity_height_ = std::clamp(
                result_selection_velocity_height_ +
                    height_acceleration * step,
                -1800.0,
                1800.0
            );
            result_selection_y_ += result_selection_velocity_y_ * step;
            result_selection_height_ +=
                result_selection_velocity_height_ * step;
        }

        const double y_error = std::abs(
            result_selection_target_y_ - result_selection_y_
        );
        const double height_error = std::abs(
            result_selection_target_height_ - result_selection_height_
        );
        if (y_error <= 0.08 &&
            height_error <= 0.08 &&
            std::abs(result_selection_velocity_y_) <= 2.0 &&
            std::abs(result_selection_velocity_height_) <= 2.0) {
            result_selection_y_ = result_selection_target_y_;
            result_selection_height_ = result_selection_target_height_;
            result_selection_velocity_y_ = 0.0;
            result_selection_velocity_height_ = 0.0;
        } else {
            animation_active = true;
        }
    }

    // Animate the selected row's contents independently from the travelling
    // highlight plate. Top and bottom margins are adjusted in opposite
    // directions, preserving each row's total height and therefore keeping the
    // list and selection geometry completely stable.
    const int row_step_count = std::max(
        1,
        static_cast<int>(std::ceil(elapsed / (1.0 / 120.0)))
    );
    const double row_step = elapsed / static_cast<double>(row_step_count);
    std::erase_if(result_row_motions_, [](const auto& motion) {
        return motion->row == nullptr || motion->content == nullptr;
    });
    for (const auto& motion : result_row_motions_) {
        const double target_lift =
            result_selection_target_visible_ && motion->row == selected_result_row_
            ? kResultRowLift
            : 0.0;

        for (int index = 0; index < row_step_count; ++index) {
            const double acceleration =
                (target_lift - motion->lift) * kResultRowLiftSpring -
                motion->velocity * kResultRowLiftDamping;
            motion->velocity = std::clamp(
                motion->velocity + acceleration * row_step,
                -720.0,
                720.0
            );
            motion->lift += motion->velocity * row_step;
        }

        const double error = std::abs(target_lift - motion->lift);
        if (error <= 0.015 && std::abs(motion->velocity) <= 0.5) {
            motion->lift = target_lift;
            motion->velocity = 0.0;
        } else {
            animation_active = true;
        }

        const int painted_lift = static_cast<int>(std::lround(std::clamp(
            motion->lift,
            -1.0,
            kResultRowLift + 1.5
        )));
        gtk_widget_set_margin_top(motion->content, 10 - painted_lift);
        gtk_widget_set_margin_bottom(motion->content, 10 + painted_lift);
    }

    const double opacity_target = result_selection_target_visible_ ? 1.0 : 0.0;
    const double opacity_blend = 1.0 -
        std::exp(-kResultSelectionOpacityResponse * elapsed);
    result_selection_opacity_ +=
        (opacity_target - result_selection_opacity_) * opacity_blend;
    if (std::abs(result_selection_opacity_ - opacity_target) <= 0.002) {
        result_selection_opacity_ = opacity_target;
    } else {
        animation_active = true;
    }

    if (result_selection_initialized_) {
        const double stretch = std::min(
            std::abs(result_selection_velocity_y_) *
                kResultSelectionStretchFactor,
            kResultSelectionMaximumStretch
        );
        const double painted_y = result_selection_y_ - stretch * 0.5;
        const double painted_height = std::max(
            1.0,
            result_selection_height_ + stretch
        );

        gtk_widget_set_margin_start(
            result_selection_indicator_,
            static_cast<int>(std::lround(result_selection_target_x_))
        );
        gtk_widget_set_margin_top(
            result_selection_indicator_,
            static_cast<int>(std::lround(painted_y))
        );
        gtk_widget_set_size_request(
            result_selection_indicator_,
            std::max(
                1,
                static_cast<int>(std::lround(result_selection_target_width_))
            ),
            std::max(1, static_cast<int>(std::lround(painted_height)))
        );
        gtk_widget_set_opacity(
            result_selection_indicator_,
            smooth_step(result_selection_opacity_)
        );
        gtk_widget_set_visible(
            result_selection_indicator_,
            result_selection_target_visible_ || result_selection_opacity_ > 0.003
        );
    }

    if (!result_selection_target_visible_ &&
        result_selection_opacity_ <= 0.002) {
        gtk_widget_set_visible(result_selection_indicator_, FALSE);
        result_selection_initialized_ = false;
        result_selection_velocity_y_ = 0.0;
        result_selection_velocity_height_ = 0.0;
        return false;
    }

    return animation_active;
}

void LauncherOverlay::activate_selected() {
    if (selected_constellation_node_ != nullptr) {
        activate_constellation_node(*selected_constellation_node_);
        return;
    }
    if (!selected_result_.has_value()) return;
    if (selected_result_->kind == services::LauncherResultKind::LauncherCommand) {
        const std::string query = selected_result_->id == "clip"
            ? ">clip "
            : selected_result_->id == "clear"
                ? ">clear"
                : selected_result_->id == "emoji" ? ">emoji " : std::string{};
        if (!query.empty()) {
            gtk_editable_set_text(GTK_EDITABLE(search_entry_), query.c_str());
            gtk_editable_set_position(GTK_EDITABLE(search_entry_), -1);
        }
        return;
    }
    if (selected_result_->kind == services::LauncherResultKind::ClipboardAction) {
        activate_clipboard_action(*selected_result_);
        return;
    }
    if ((selected_result_->kind == services::LauncherResultKind::Command ||
         selected_result_->kind == services::LauncherResultKind::Action) &&
        command_receipts_.execute(*selected_result_)) {
        return;
    }
    if (service_.activate(*selected_result_)) hide();
}

void LauncherOverlay::activate_result(std::size_t index) {
    if (index >= current_results_.size()) return;
    set_selected_result(&current_results_[index]);
    activate_selected();
}

bool LauncherOverlay::handle_key(guint keyval, GdkModifierType modifiers) {
    if (keyval == GDK_KEY_Escape) {
        const char* text = gtk_editable_get_text(GTK_EDITABLE(search_entry_));
        if (text != nullptr && *text != '\0') {
            gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
            return true;
        }
        hide();
        return true;
    }

    if ((modifiers & GDK_CONTROL_MASK) != 0 &&
        (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        clear_constellation_selection();
        gtk_widget_grab_focus(search_entry_);
        gtk_editable_select_region(GTK_EDITABLE(search_entry_), 0, -1);
        return true;
    }

    const char* query_text = gtk_editable_get_text(GTK_EDITABLE(search_entry_));
    const bool searching = query_text != nullptr &&
        std::string_view(query_text).find_first_not_of(" \t\n\r") != std::string_view::npos;

    if (!searching) {
        switch (keyval) {
        case GDK_KEY_Left:
        case GDK_KEY_KP_Left:
            return navigate_constellation(SpatialDirection::Left);
        case GDK_KEY_Right:
        case GDK_KEY_KP_Right:
            return navigate_constellation(SpatialDirection::Right);
        case GDK_KEY_Up:
        case GDK_KEY_KP_Up:
            return navigate_constellation(SpatialDirection::Up);
        case GDK_KEY_Down:
        case GDK_KEY_KP_Down:
            return navigate_constellation(SpatialDirection::Down);
        default:
            break;
        }
    }

    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        activate_selected();
        return true;
    }

    const bool result_navigation_key = keyval == GDK_KEY_Down ||
        keyval == GDK_KEY_Up || keyval == GDK_KEY_Page_Down ||
        keyval == GDK_KEY_Page_Up;
    if (result_navigation_key && !current_results_.empty()) {
        GtkListBoxRow* selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(results_list_));
        int index = selected != nullptr ? gtk_list_box_row_get_index(selected) : 0;
        if (keyval == GDK_KEY_Down) ++index;
        else if (keyval == GDK_KEY_Up) --index;
        else if (keyval == GDK_KEY_Page_Down) index += 7;
        else index -= 7;

        if (search_mode_ == SearchMode::Clipboard && index >= 0 &&
            static_cast<std::size_t>(index) >= current_results_.size()) {
            static_cast<void>(append_next_clipboard_page(
                static_cast<std::size_t>(index) + 1
            ));
        } else if (search_mode_ == SearchMode::Emoji && index >= 0 &&
            static_cast<std::size_t>(index) >= current_results_.size()) {
            static_cast<void>(append_next_emoji_page(
                static_cast<std::size_t>(index) + 1
            ));
        }

        index = std::clamp(index, 0, static_cast<int>(current_results_.size()) - 1);
        GtkListBoxRow* target = gtk_list_box_get_row_at_index(
            GTK_LIST_BOX(results_list_),
            index
        );
        gtk_list_box_select_row(GTK_LIST_BOX(results_list_), target);
        ensure_result_row_visible(target);
        return true;
    }

    constexpr GdkModifierType blocked_text_modifiers = static_cast<GdkModifierType>(
        GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK
    );
    if (!searching && selected_constellation_node_ != nullptr &&
        (modifiers & blocked_text_modifiers) == 0) {
        const gunichar character = gdk_keyval_to_unicode(keyval);
        if (character != 0 && g_unichar_isprint(character)) {
            char utf8[7]{};
            const int length = g_unichar_to_utf8(character, utf8);
            clear_constellation_selection();
            gtk_widget_grab_focus(search_entry_);
            int position = gtk_editable_get_position(GTK_EDITABLE(search_entry_));
            gtk_editable_insert_text(
                GTK_EDITABLE(search_entry_),
                utf8,
                length,
                &position
            );
            gtk_editable_set_position(GTK_EDITABLE(search_entry_), position);
            return true;
        }
    }

    return false;
}

bool LauncherOverlay::search_query_active() const {
    if (search_entry_ == nullptr) return false;
    const char* text = gtk_editable_get_text(GTK_EDITABLE(search_entry_));
    return text != nullptr &&
        std::string_view(text).find_first_not_of(" \t\n\r") !=
            std::string_view::npos;
}

void LauncherOverlay::schedule_central_frame() {
    if (central_tick_id_ != 0 || root_ == nullptr) return;
    central_last_frame_time_ = 0;
    central_tick_id_ = gtk_widget_add_tick_callback(
        root_,
        +[](GtkWidget*, GdkFrameClock* frame_clock, gpointer data) -> gboolean {
            auto* overlay = static_cast<LauncherOverlay*>(data);
            if (overlay->advance_central_frame(frame_clock)) {
                return G_SOURCE_CONTINUE;
            }
            overlay->central_tick_id_ = 0;
            overlay->central_last_frame_time_ = 0;
            return G_SOURCE_REMOVE;
        },
        this,
        nullptr
    );
}

void LauncherOverlay::apply_central_motion() {
    if (dismiss_ == nullptr || centre_column_ == nullptr || centre_shell_ == nullptr ||
        wallpaper_frame_ == nullptr || search_entry_ == nullptr) {
        return;
    }

    const double backdrop = smooth_step(interval_progress(central_progress_, 0.0, 0.44));
    const double frame = smooth_step(interval_progress(central_progress_, 0.04, 0.68));
    const double aperture = ease_out_cubic(
        interval_progress(central_progress_, 0.12, 0.92)
    );
    const double search = smooth_step(interval_progress(central_progress_, 0.46, 1.0));

    gtk_widget_set_opacity(dismiss_, backdrop);
    gtk_widget_set_opacity(centre_column_, frame);
    gtk_widget_set_margin_top(
        centre_column_,
        static_cast<int>(std::lround(interpolate(
            kCentreStartTopMargin,
            kCentreFinalTopMargin,
            frame
        )))
    );
    gtk_widget_set_size_request(
        centre_shell_,
        static_cast<int>(std::lround(interpolate(
            kCentreStartWidth,
            kCentreFinalWidth,
            frame
        ))),
        kCentreHeight
    );
    gtk_widget_set_size_request(
        wallpaper_frame_,
        static_cast<int>(std::lround(interpolate(
            kApertureStartWidth,
            kApertureFinalWidth,
            aperture
        ))),
        static_cast<int>(std::lround(interpolate(
            kApertureStartHeight,
            kApertureFinalHeight,
            aperture
        )))
    );
    gtk_widget_set_opacity(
        wallpaper_picture_,
        interpolate(0.18, 1.0, aperture)
    );
    gtk_widget_set_size_request(
        search_entry_,
        static_cast<int>(std::lround(interpolate(
            kSearchStartWidth,
            kSearchFinalWidth,
            search
        ))),
        50
    );
    gtk_widget_set_opacity(search_entry_, interpolate(0.10, 1.0, search));
    gtk_widget_set_margin_bottom(
        search_entry_,
        static_cast<int>(std::lround(interpolate(10.0, 0.0, search)))
    );

    if (activation_sweep_ != nullptr) {
        const double sweep = interval_progress(central_progress_, 0.56, 0.96);
        const double sweep_opacity = central_target_visible_
            ? std::sin(sweep * std::numbers::pi) * (sweep > 0.0 && sweep < 1.0)
            : 0.0;
        gtk_widget_set_visible(activation_sweep_, sweep_opacity > 0.01);
        gtk_widget_set_opacity(activation_sweep_, clamp_unit(sweep_opacity));
        gtk_widget_set_margin_start(
            activation_sweep_,
            static_cast<int>(std::lround(interpolate(38.0, 518.0, sweep)))
        );
        gtk_widget_set_margin_bottom(activation_sweep_, 1);
    }
}

bool LauncherOverlay::advance_central_frame(GdkFrameClock* frame_clock) {
    const gint64 frame_time = gdk_frame_clock_get_frame_time(frame_clock);
    double elapsed = 1.0 / 60.0;
    if (central_last_frame_time_ != 0) {
        elapsed = static_cast<double>(frame_time - central_last_frame_time_) /
            1'000'000.0;
    }
    central_last_frame_time_ = frame_time;
    elapsed = std::clamp(elapsed, 1.0 / 240.0, 0.05);

    const double target = central_target_visible_ ? 1.0 : 0.0;
    const double rate = central_target_visible_ ? kCentralOpenRate : kCentralCloseRate;
    if (central_progress_ < target) {
        central_progress_ = std::min(target, central_progress_ + elapsed * rate);
    } else if (central_progress_ > target) {
        central_progress_ = std::max(target, central_progress_ - elapsed * rate);
    }

    apply_central_motion();

    if (central_target_visible_ &&
        central_progress_ >= kConstellationRevealThreshold &&
        !search_query_active()) {
        set_constellation_visible(true);
    } else if (!central_target_visible_) {
        set_constellation_visible(false);
    }

    if (std::abs(central_progress_ - target) > 0.0001) return true;
    if (!central_target_visible_) finish_central_hide();
    return false;
}

void LauncherOverlay::finish_central_hide() {
    if (constellation_tick_id_ != 0 && root_ != nullptr) {
        gtk_widget_remove_tick_callback(root_, constellation_tick_id_);
        constellation_tick_id_ = 0;
        constellation_last_frame_time_ = 0;
    }
    constellation_target_visible_ = false;
    clear_constellation_selection();
    for (const auto& node : constellation_nodes_) {
        node->opacity = 0.0;
        node->visibility_delay = 0.0;
        gtk_widget_set_opacity(node->widget, 0.0);
        gtk_widget_set_can_target(node->widget, FALSE);
    }
    if (constellation_canvas_ != nullptr) {
        gtk_widget_set_visible(constellation_canvas_, FALSE);
    }
    if (activation_sweep_ != nullptr) {
        gtk_widget_set_visible(activation_sweep_, FALSE);
    }
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    leave_clipboard_mode();
    leave_emoji_mode();
}

void LauncherOverlay::toggle() {
    if (gtk_widget_get_visible(GTK_WIDGET(window_)) && central_target_visible_) {
        hide();
    } else {
        show();
    }
}

void LauncherOverlay::show() {
    const bool already_presented = gtk_widget_get_visible(GTK_WIDGET(window_));
    central_target_visible_ = true;
    gtk_widget_set_sensitive(root_, TRUE);

    if (!already_presented) {
        central_progress_ = 0.0;
        central_last_frame_time_ = 0;
        constellation_target_visible_ = false;
        refresh_wallpaper();
        refresh_idle_content();
        gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
        on_search_changed();
        apply_central_motion();
        gtk_window_present(window_);
        gtk_widget_grab_focus(search_entry_);
        g_idle_add(+[](gpointer data) -> gboolean {
            static_cast<LauncherOverlay*>(data)->layout_constellation();
            return G_SOURCE_REMOVE;
        }, this);
    }

    schedule_central_frame();
}

void LauncherOverlay::show_with_query(std::string query) {
    std::string clipboard_filter;
    if (search_mode_ == SearchMode::Clipboard &&
        parse_clipboard_query(query, clipboard_filter)) {
        // A repeated SUPER+V should re-read cliphist even when the entry text is
        // already exactly ">clip ", which would otherwise emit no changed signal.
        leave_clipboard_mode();
    }

    show();
    gtk_editable_set_text(GTK_EDITABLE(search_entry_), query.c_str());
    gtk_widget_grab_focus(search_entry_);

    const auto collapse_selection_at_end = [](GtkEditable* editable) {
        if (!GTK_IS_EDITABLE(editable)) return;
        const char* text = gtk_editable_get_text(editable);
        const auto length = static_cast<int>(g_utf8_strlen(text != nullptr ? text : "", -1));
        gtk_editable_select_region(editable, length, length);
        gtk_editable_set_position(editable, length);
    };

    // GtkEntry may select all text while the newly presented launcher receives
    // focus. Collapse that selection immediately, then once more from the idle
    // queue after the Wayland focus/presentation round-trip has completed.
    collapse_selection_at_end(GTK_EDITABLE(search_entry_));
    g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        +[](gpointer data) -> gboolean {
            auto* editable = GTK_EDITABLE(data);
            if (GTK_IS_EDITABLE(editable)) {
                const char* text = gtk_editable_get_text(editable);
                const auto length = static_cast<int>(
                    g_utf8_strlen(text != nullptr ? text : "", -1)
                );
                gtk_editable_select_region(editable, length, length);
                gtk_editable_set_position(editable, length);
            }
            return G_SOURCE_REMOVE;
        },
        g_object_ref(search_entry_),
        g_object_unref
    );
    on_search_changed();
}

void LauncherOverlay::hide() {
    if (!gtk_widget_get_visible(GTK_WIDGET(window_)) || !central_target_visible_) {
        return;
    }

    central_target_visible_ = false;
    clear_constellation_selection();
    set_constellation_visible(false);
    gtk_widget_set_sensitive(root_, FALSE);
    schedule_central_frame();
}

} // namespace realmheart::ui
