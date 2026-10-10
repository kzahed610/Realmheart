#include "wallpaper-native/source/WallpaperMediaProbe.hpp"

#include "wallpaper-native/NativeWallpaperContracts.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#if REALMHEART_HAVE_LIVE_WALLPAPER
// FFmpeg's public headers do not wrap themselves in extern "C", so a C++
// translation unit must do it or every symbol mangles and fails to link.
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::wallpaper_native {

namespace {

// Classification never throws and never guesses: every path ends in either a
// media type or a reason the caller can report.
WallpaperMediaProbe unsupported(std::string reason) {
    return WallpaperMediaProbe{
        WallpaperMediaType::Unsupported,
        std::string{},
        std::move(reason)
    };
}

WallpaperMediaProbe image_source() {
    return WallpaperMediaProbe{
        WallpaperMediaType::StaticImage,
        std::string{},
        std::string{}
    };
}

WallpaperMediaProbe check_file_budget(const std::string& path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status)) {
        return unsupported("wallpaper file does not exist");
    }
    if (!std::filesystem::is_regular_file(status)) {
        return unsupported("wallpaper path is not a regular file");
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return unsupported("wallpaper file size could not be read");
    }
    if (size > kNativeMaxSourceBytes) {
        return unsupported("wallpaper file exceeds the decode budget");
    }
    return image_source();
}

bool image_probe_succeeds(const std::string& path) {
    int width = 0;
    int height = 0;
    return gdk_pixbuf_get_file_info(path.c_str(), &width, &height) != FALSE &&
           width > 0 && height > 0;
}

#if REALMHEART_HAVE_LIVE_WALLPAPER

// Bound the probing work so classification stays cheap on a large library:
// headers and a little data, never a full read of the file.
constexpr std::int64_t kProbeSizeBytes = 8LL * 1024LL * 1024LL;
constexpr std::int64_t kMaxAnalyzeMicros = 2LL * AV_TIME_BASE;

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(63, 0, 100)
// FFmpeg 8 replaced the deprecated avio_free_context with avio_context_free.
constexpr auto release_avio_context = &avio_context_free;
#else
void release_avio_context(AVIOContext** context) {
    if (context != nullptr && *context != nullptr) {
        avio_free_context(*context);
        *context = nullptr;
    }
}
#endif

const char* ffmpeg_error_text(int code) {
    static thread_local std::vector<char> buffer(AV_ERROR_MAX_STRING_SIZE, '\0');
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return "unknown FFmpeg error";
    }
    return buffer.data();
}

std::string trim_trailing_zeros(std::string text) {
    if (text.find('.') == std::string::npos) return text;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

std::string describe_stream(const AVStream* stream) {
    if (stream == nullptr) return {};

    const AVCodecParameters* codec = stream->codecpar;
    std::string detail =
        (codec != nullptr && codec->codec_id != AV_CODEC_ID_NONE)
            ? avcodec_get_name(codec->codec_id)
            : std::string{"unknown codec"};

    if (codec != nullptr && codec->width > 0 && codec->height > 0) {
        detail += " " + std::to_string(codec->width) + "x" +
                  std::to_string(codec->height);
    }

    AVRational rate{0, 1};
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        rate = stream->avg_frame_rate;
    } else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
        rate = stream->r_frame_rate;
    }
    if (rate.num > 0 && rate.den > 0) {
        const double fps =
            static_cast<double>(rate.num) / static_cast<double>(rate.den);
        char rendered[32]{};
        std::snprintf(rendered, sizeof(rendered), "%.6g", fps);
        detail += " " + trim_trailing_zeros(std::string{rendered}) + "fps";
    }
    return detail;
}

// Demuxes an in-memory payload so owned bytes can be classified without ever
// writing them to disk. Open/close are a pair; every failure path leaves
// nothing owned by the caller.
class MemoryDemuxer {
public:
    explicit MemoryDemuxer(std::string_view bytes)
        : bytes_(bytes),
          state_{reinterpret_cast<const unsigned char*>(bytes.data()),
                 bytes.size(),
                 0} {}

    [[nodiscard]] bool open(AVFormatContext** format) {
        *format = nullptr;

        constexpr int io_buffer_size = 4096;
        io_buffer_ = static_cast<unsigned char*>(av_malloc(io_buffer_size));
        if (io_buffer_ == nullptr) return false;

        io_ = avio_alloc_context(
            io_buffer_,
            io_buffer_size,
            0,
            &state_,
            &MemoryDemuxer::read_packet,
            nullptr,
            &MemoryDemuxer::seek_packet
        );
        if (io_ == nullptr) {
            av_free(io_buffer_);
            io_buffer_ = nullptr;
            return false;
        }

        AVFormatContext* context = avformat_alloc_context();
        if (context == nullptr) {
            // release_avio_context also frees the buffer it was given.
            release_avio_context(&io_);
            io_ = nullptr;
            io_buffer_ = nullptr;
            return false;
        }

        // From here the format context owns the AVIOContext, and frees it on
        // both a failed open and on close.
        context->pb = io_;
        context->probesize = kProbeSizeBytes;
        context->max_analyze_duration = kMaxAnalyzeMicros;

        if (avformat_open_input(&context, "pipe", nullptr, nullptr) < 0) {
            io_ = nullptr;
            io_buffer_ = nullptr;
            avformat_close_input(&context);
            return false;
        }
        *format = context;
        return true;
    }

    void close(AVFormatContext* format) const {
        if (format != nullptr) avformat_close_input(&format);
    }

private:
    struct MemoryState {
        const unsigned char* data;
        std::size_t size;
        std::size_t offset;
    };

    static int read_packet(void* opaque, unsigned char* buffer, int buffer_size) {
        auto* state = static_cast<MemoryState*>(opaque);
        if (buffer_size <= 0) return 0;
        const std::size_t remaining = state->size - state->offset;
        if (remaining == 0) return AVERROR_EOF;
        const std::size_t count =
            std::min<std::size_t>(remaining, static_cast<std::size_t>(buffer_size));
        std::memcpy(buffer, state->data + state->offset, count);
        state->offset += count;
        return static_cast<int>(count);
    }

    static std::int64_t seek_packet(void* opaque, std::int64_t offset, int whence) {
        auto* state = static_cast<MemoryState*>(opaque);
        if ((whence & AVSEEK_SIZE) != 0) {
            return static_cast<std::int64_t>(state->size);
        }
        std::int64_t target = 0;
        switch (whence) {
            case SEEK_SET:
                target = offset;
                break;
            case SEEK_CUR:
                target = static_cast<std::int64_t>(state->offset) + offset;
                break;
            case SEEK_END:
                target = static_cast<std::int64_t>(state->size) + offset;
                break;
            default:
                return -1;
        }
        if (target < 0 || static_cast<std::uint64_t>(target) > state->size) {
            return -1;
        }
        state->offset = static_cast<std::size_t>(target);
        return target;
    }

    std::string_view bytes_;
    MemoryState state_;
    unsigned char* io_buffer_ = nullptr;
    AVIOContext* io_ = nullptr;
};

#endif

} // namespace

WallpaperMediaProbe probe_wallpaper_media(const std::string& path) {
    if (path.empty()) return unsupported("wallpaper path is empty");

    const auto budget = check_file_budget(path);
    if (budget.type == WallpaperMediaType::Unsupported) return budget;

    // Content first, extension never. An image wins even when mislabelled, and
    // including formats whose first frame GdkPixbuf can decode.
    if (image_probe_succeeds(path)) return image_source();

#if REALMHEART_HAVE_LIVE_WALLPAPER
    AVFormatContext* format = avformat_alloc_context();
    if (format == nullptr) {
        return unsupported("unable to allocate an FFmpeg format context");
    }
    format->probesize = kProbeSizeBytes;
    format->max_analyze_duration = kMaxAnalyzeMicros;

    const int open_code = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (open_code < 0) {
        const std::string reason =
            std::string{"unable to open wallpaper as media: "} + ffmpeg_error_text(open_code);
        avformat_close_input(&format);
        return unsupported(reason);
    }
    if (avformat_find_stream_info(format, nullptr) < 0) {
        avformat_close_input(&format);
        return unsupported("unable to read wallpaper streams");
    }

    const int video_index = av_find_best_stream(
        format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0
    );
    if (video_index < 0) {
        // Audio-only and stream-less containers are equally unsupported.
        avformat_close_input(&format);
        return unsupported("wallpaper contains no video stream");
    }

    WallpaperMediaProbe probe;
    probe.type = WallpaperMediaType::Video;
    probe.detail = describe_stream(format->streams[video_index]);
    avformat_close_input(&format);
    return probe;
#else
    return unsupported(
        "live wallpaper support is not compiled into this build; "
        "only static images are available"
    );
#endif
}

WallpaperMediaProbe probe_wallpaper_media_bytes(std::string_view bytes) {
    if (bytes.empty()) return unsupported("owned wallpaper bytes are empty");
    if (bytes.size() > kNativeMaxSourceBytes) {
        return unsupported("owned wallpaper bytes exceed the decode budget");
    }

#if REALMHEART_HAVE_LIVE_WALLPAPER
    MemoryDemuxer demuxer(bytes);
    AVFormatContext* format = nullptr;
    if (demuxer.open(&format)) {
        const int video_index = avformat_find_stream_info(format, nullptr) < 0
            ? -1
            : av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        demuxer.close(format);
        if (video_index >= 0) {
            return WallpaperMediaProbe{
                WallpaperMediaType::Video,
                std::string{},
                std::string{}
            };
        }
    }
#endif
    // Not a video payload. The image decoder decides whether it is an image,
    // so its own error stays the message an undecodable payload produces.
    return WallpaperMediaProbe{
        WallpaperMediaType::Unsupported,
        std::string{},
        std::string{}
    };
}

} // namespace realmheart::wallpaper_native
